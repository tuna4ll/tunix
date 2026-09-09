/* Minimal AF_NETLINK support. */

#include <stddef.h>
#include <stdint.h>
#include "../include/heap.h"
#include "../include/kstring.h"
#include "../include/cred.h"
#include "../include/net/netlink.h"
#include "../include/net/net.h"
#include "../include/process.h"


#define EAGAIN 11
#define ENOENT 2
#define EINVAL 22
#define EOPNOTSUPP 95
#define ENODEV 19

#define NL_MSG_PEEK 0x2
#define NL_MSG_TRUNC 0x20

/* Netlink constants. */

#define NETLINK_GENERIC 16

#define NLMSG_NOOP 1
#define NLMSG_ERROR 2
#define NLMSG_DONE 3

#define NLM_F_REQUEST 0x001
#define NLM_F_MULTI 0x002
#define NLM_F_ACK 0x004
#define NLM_F_DUMP 0x300

#define RTM_NEWLINK 16
#define RTM_GETLINK 18
#define RTM_NEWADDR 20
#define RTM_DELADDR 21
#define RTM_GETADDR 22
#define RTM_NEWROUTE 24
#define RTM_DELROUTE 25
#define RTM_GETROUTE 26

#define IFLA_ADDRESS 1
#define IFLA_BROADCAST 2
#define IFLA_IFNAME 3
#define IFLA_MTU 4
#define IFLA_TXQLEN 13

#define IFA_ADDRESS 1
#define IFA_LOCAL 2
#define IFA_LABEL 3
#define IFA_BROADCAST 4

#define RTA_DST 1
#define RTA_OIF 4
#define RTA_GATEWAY 5
#define RTA_PREFSRC 7
#define RTA_TABLE 15

#define ARPHRD_ETHER 1
#define ARPHRD_LOOPBACK 772

#define IFF_UP 0x1
#define IFF_BROADCAST 0x2
#define IFF_LOOPBACK 0x8
#define IFF_RUNNING 0x40
#define IFF_MULTICAST 0x1000
#define IFF_LOWER_UP 0x10000

#define NL_AF_UNSPEC 0
#define NL_AF_INET 2

#define RT_TABLE_MAIN 254
#define RT_SCOPE_UNIVERSE 0
#define RT_SCOPE_LINK 253
#define RTPROT_BOOT 3
#define RTPROT_KERNEL 2
#define RTN_UNICAST 1

#define NETLINK_INDEX_LO NET_IFINDEX_LO
#define NETLINK_INDEX_ETH0 NET_IFINDEX_ETH0

struct nlmsghdr {
    uint32_t nlmsg_len;
    uint16_t nlmsg_type;
    uint16_t nlmsg_flags;
    uint32_t nlmsg_seq;
    uint32_t nlmsg_pid;
} __attribute__((packed));

struct nlmsgerr {
    int32_t error;
    struct nlmsghdr msg;
} __attribute__((packed));

struct rtattr {
    uint16_t rta_len;
    uint16_t rta_type;
} __attribute__((packed));

struct ifinfomsg {
    uint8_t ifi_family;
    uint8_t ifi_pad;
    uint16_t ifi_type;
    int32_t ifi_index;
    uint32_t ifi_flags;
    uint32_t ifi_change;
} __attribute__((packed));

struct ifaddrmsg {
    uint8_t ifa_family;
    uint8_t ifa_prefixlen;
    uint8_t ifa_flags;
    uint8_t ifa_scope;
    uint32_t ifa_index;
} __attribute__((packed));

struct rtmsg {
    uint8_t rtm_family;
    uint8_t rtm_dst_len;
    uint8_t rtm_src_len;
    uint8_t rtm_tos;
    uint8_t rtm_table;
    uint8_t rtm_protocol;
    uint8_t rtm_scope;
    uint8_t rtm_type;
    uint32_t rtm_flags;
} __attribute__((packed));

#define NLMSG_ALIGN(len) (((len) + 3U) & ~3U)

/* Netlink sockets queue datagrams. */
struct netlink_datagram {
    struct netlink_datagram *next;
    /* Source metadata belongs to each datagram. */
    uint32_t source_portid;
    uint32_t source_groups;
    struct netlink_credentials source_credentials;
    size_t length;
    uint8_t data[];
};

struct netlink_socket {
    int refs;
    int protocol;
    uint32_t portid;
    int bound;
    /* Multicast group mask. */
    uint32_t groups;
    int passcred;
    struct netlink_credentials last_credentials;
    struct netlink_datagram *rx_head;
    struct netlink_datagram *rx_tail;
    /* Registry link. */
    struct netlink_socket *registry_next;
};

static uint32_t netlink_next_portid = 0;
static struct netlink_socket *netlink_registry = NULL;

struct netlink_socket *netlink_socket_create(int protocol) {
    if (protocol != TUNIX_NETLINK_ROUTE && protocol != TUNIX_NETLINK_SOCK_DIAG &&
        protocol != TUNIX_NETLINK_KOBJECT_UEVENT && protocol != NETLINK_GENERIC) return NULL;
    struct netlink_socket *socket = (struct netlink_socket *)kmalloc(sizeof(*socket));
    if (!socket) return NULL;
    memset(socket, 0, sizeof(*socket));
    socket->refs = 1;
    socket->protocol = protocol;
    socket->registry_next = netlink_registry;
    netlink_registry = socket;
    return socket;
}

void netlink_socket_ref(struct netlink_socket *socket) {
    if (socket) socket->refs++;
}

void netlink_socket_unref(struct netlink_socket *socket) {
    if (!socket || socket->refs <= 0) return;
    if (--socket->refs != 0) return;
    for (struct netlink_socket **at = &netlink_registry; *at; at = &(*at)->registry_next) {
        if (*at == socket) { *at = socket->registry_next; break; }
    }
    while (socket->rx_head) {
        struct netlink_datagram *dead = socket->rx_head;
        socket->rx_head = dead->next;
        kfree(dead);
    }
    kfree(socket);
}

static uint32_t netlink_assign_portid(struct netlink_socket *socket) {
    if (!socket->portid) socket->portid = ++netlink_next_portid;
    return socket->portid;
}

int netlink_socket_bind(struct netlink_socket *socket, const void *address, size_t length) {
    if (!socket) return -EINVAL;
    const struct tunix_sockaddr_nl *nl = (const struct tunix_sockaddr_nl *)address;
    if (nl && length >= sizeof(*nl) && nl->pid) socket->portid = nl->pid;
    else netlink_assign_portid(socket);
    /* Zero groups means no subscription. */
    if (nl && length >= sizeof(*nl)) socket->groups = nl->groups;
    socket->bound = 1;
    return 0;
}

void netlink_socket_set_passcred(struct netlink_socket *socket, int on) {
    if (socket) socket->passcred = on ? 1 : 0;
}

int netlink_socket_get_passcred(struct netlink_socket *socket) {
    return socket ? socket->passcred : 0;
}

void netlink_socket_last_credentials(struct netlink_socket *socket,
                                     struct netlink_credentials *out) {
    if (!out) return;
    if (!socket) { memset(out, 0, sizeof(*out)); return; }
    *out = socket->last_credentials;
}

int netlink_socket_getsockname(struct netlink_socket *socket, void *address, size_t *length) {
    if (!socket || !address || !length) return -EINVAL;
    struct tunix_sockaddr_nl nl;
    memset(&nl, 0, sizeof(nl));
    nl.family = TUNIX_AF_NETLINK;
    nl.pid = netlink_assign_portid(socket);
    size_t copy = *length < sizeof(nl) ? *length : sizeof(nl);
    memcpy(address, &nl, copy);
    *length = sizeof(nl);
    return 0;
}

/* Response builder. */

struct nl_builder {
    uint8_t *buf;
    size_t cap;
    size_t len;
    size_t msg_start;
    int overflow;
};

static void nl_pad(struct nl_builder *b) {
    size_t aligned = NLMSG_ALIGN(b->len);
    while (b->len < aligned && b->len < b->cap) b->buf[b->len++] = 0;
}

static struct nlmsghdr *nl_msg_begin(struct nl_builder *b, uint16_t type, uint16_t flags,
                                     uint32_t seq, uint32_t pid,
                                     const void *family_header, size_t family_length) {
    nl_pad(b);
    if (b->len + sizeof(struct nlmsghdr) + family_length > b->cap) { b->overflow = 1; return NULL; }
    b->msg_start = b->len;
    struct nlmsghdr *header = (struct nlmsghdr *)(b->buf + b->len);
    memset(header, 0, sizeof(*header));
    /* Keep partial headers parseable. */
    header->nlmsg_len = (uint32_t)(sizeof(*header) + family_length);
    header->nlmsg_type = type;
    header->nlmsg_flags = flags;
    header->nlmsg_seq = seq;
    header->nlmsg_pid = pid;
    b->len += sizeof(*header);
    if (family_length) {
        memcpy(b->buf + b->len, family_header, family_length);
        b->len += family_length;
    }
    return header;
}

static void nl_attr(struct nl_builder *b, uint16_t type, const void *data, size_t length) {
    nl_pad(b);
    size_t total = sizeof(struct rtattr) + length;
    if (b->len + NLMSG_ALIGN(total) > b->cap) { b->overflow = 1; return; }
    struct rtattr *attr = (struct rtattr *)(b->buf + b->len);
    attr->rta_len = (uint16_t)total;
    attr->rta_type = type;
    if (length) memcpy(b->buf + b->len + sizeof(*attr), data, length);
    /* Align attributes while preserving rta_len. */
    size_t padded = NLMSG_ALIGN(total);
    for (size_t pad = total; pad < padded; pad++) b->buf[b->len + pad] = 0;
    b->len += padded;
}

static void nl_msg_end(struct nl_builder *b, struct nlmsghdr *header) {
    if (!header) return;
    if (b->overflow) {
        /* Drop partial messages. */
        b->len = b->msg_start;
        return;
    }
    header->nlmsg_len = (uint32_t)(b->len - b->msg_start);
}

static void nl_attr_u32(struct nl_builder *b, uint16_t type, uint32_t value) {
    nl_attr(b, type, &value, sizeof(value));
}

static void nl_put_done(struct nl_builder *b, uint32_t seq, uint32_t pid) {
    int32_t code = 0;
    struct nlmsghdr *header = nl_msg_begin(b, NLMSG_DONE, NLM_F_MULTI, seq, pid,
                                           &code, sizeof(code));
    nl_msg_end(b, header);
}

static void nl_put_error(struct nl_builder *b, uint32_t seq, uint32_t pid,
                         const struct nlmsghdr *request, int32_t error) {
    struct nlmsgerr body;
    memset(&body, 0, sizeof(body));
    body.error = error;
    if (request) body.msg = *request;
    struct nlmsghdr *header = nl_msg_begin(b, NLMSG_ERROR, 0, seq, pid, &body, sizeof(body));
    nl_msg_end(b, header);
}

/* Interface projection. */

static uint8_t netmask_prefix(uint32_t netmask_network_order) {
    /* Count the network-order prefix bits. */
    uint8_t prefix = 0;
    const uint8_t *bytes = (const uint8_t *)&netmask_network_order;
    for (int i = 0; i < 4; i++) {
        uint8_t byte = bytes[i];
        while (byte & 0x80U) { prefix++; byte = (uint8_t)(byte << 1); }
    }
    return prefix;
}

static void emit_link(struct nl_builder *b, uint32_t seq, uint32_t pid, uint16_t msg_flags,
                      int index, const char *name, uint16_t arptype, uint32_t flags,
                      uint32_t mtu, const uint8_t *mac, int mac_length) {
    struct ifinfomsg info;
    memset(&info, 0, sizeof(info));
    info.ifi_family = NL_AF_UNSPEC;
    info.ifi_type = arptype;
    info.ifi_index = index;
    info.ifi_flags = flags;
    struct nlmsghdr *header = nl_msg_begin(b, RTM_NEWLINK, msg_flags, seq, pid,
                                           &info, sizeof(info));
    nl_attr(b, IFLA_IFNAME, name, strlen(name) + 1);
    nl_attr(b, IFLA_MTU, &mtu, sizeof(mtu));
    nl_attr_u32(b, IFLA_TXQLEN, 1000U);
    if (mac_length) {
        nl_attr(b, IFLA_ADDRESS, mac, (size_t)mac_length);
        uint8_t broadcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
        nl_attr(b, IFLA_BROADCAST, broadcast, sizeof(broadcast));
    }
    nl_msg_end(b, header);
}

/* Kernel interface descriptions. */
struct link_description {
    int index;
    const char *name;
    uint16_t arptype;
    uint32_t flags;
    uint32_t mtu;
    const uint8_t *mac;
    int mac_length;
};

static unsigned collect_links(struct link_description *links,
                              uint8_t loopback_mac[6]) {
    memset(loopback_mac, 0, 6);
    links[0].index = NETLINK_INDEX_LO;
    links[0].name = "lo";
    links[0].arptype = ARPHRD_LOOPBACK;
    links[0].flags = IFF_UP | IFF_LOOPBACK | IFF_RUNNING;
    links[0].mtu = 65536U;
    links[0].mac = loopback_mac;
    links[0].mac_length = 0;

    const struct net_config *config = net_get_config();
    uint32_t flags = IFF_BROADCAST | IFF_MULTICAST;
    if (config->interface_up) flags |= IFF_UP;
    if (config->link_up) flags |= IFF_RUNNING | IFF_LOWER_UP;
    links[1].index = NETLINK_INDEX_ETH0;
    links[1].name = "eth0";
    links[1].arptype = ARPHRD_ETHER;
    links[1].flags = flags;
    links[1].mtu = 1500U;
    links[1].mac = config->mac;
    links[1].mac_length = 6;
    return 2;
}

/* Read IFLA_IFNAME from a request. */
static const char *request_link_name(const struct nlmsghdr *request) {
    if (request->nlmsg_len < sizeof(*request) + sizeof(struct ifinfomsg)) return NULL;
    const uint8_t *base = (const uint8_t *)request;
    size_t offset = sizeof(*request) + sizeof(struct ifinfomsg);
    while (offset + sizeof(struct rtattr) <= request->nlmsg_len) {
        const struct rtattr *attr = (const struct rtattr *)(base + offset);
        if (attr->rta_len < sizeof(*attr) ||
            offset + attr->rta_len > request->nlmsg_len) break;
        if (attr->rta_type == IFLA_IFNAME)
            return (const char *)(base + offset + sizeof(*attr));
        offset += NLMSG_ALIGN(attr->rta_len);
    }
    return NULL;
}

/* Dump all links or answer one lookup. */
static int dump_links(struct nl_builder *b, uint32_t seq, uint32_t pid,
                      const struct nlmsghdr *request) {
    struct link_description links[2];
    uint8_t loopback_mac[6];
    unsigned count = collect_links(links, loopback_mac);

    if (request->nlmsg_flags & NLM_F_DUMP) {
        for (unsigned index = 0; index < count; index++)
            emit_link(b, seq, pid, NLM_F_MULTI, links[index].index, links[index].name,
                      links[index].arptype, links[index].flags, links[index].mtu,
                      links[index].mac, links[index].mac_length);
        return 1;
    }

    const struct ifinfomsg *info = (const struct ifinfomsg *)((const uint8_t *)request +
                                                             sizeof(*request));
    int wanted_index = request->nlmsg_len >= sizeof(*request) + sizeof(*info) ?
        info->ifi_index : 0;
    const char *wanted_name = request_link_name(request);

    for (unsigned index = 0; index < count; index++) {
        if (wanted_index && links[index].index != wanted_index) continue;
        if (wanted_name && strcmp(links[index].name, wanted_name) != 0) continue;
        emit_link(b, seq, pid, 0, links[index].index, links[index].name,
                  links[index].arptype, links[index].flags, links[index].mtu,
                  links[index].mac, links[index].mac_length);
        return 0;
    }
    nl_put_error(b, seq, pid, request, -ENODEV);
    return 0;
}

static void emit_addr(struct nl_builder *b, uint32_t seq, uint32_t pid, int index,
                      const char *label, uint8_t prefix, uint8_t scope,
                      uint32_t address_network_order) {
    struct ifaddrmsg addr;
    memset(&addr, 0, sizeof(addr));
    addr.ifa_family = NL_AF_INET;
    addr.ifa_prefixlen = prefix;
    addr.ifa_scope = scope;
    addr.ifa_index = (uint32_t)index;
    struct nlmsghdr *header = nl_msg_begin(b, RTM_NEWADDR, NLM_F_MULTI, seq, pid,
                                           &addr, sizeof(addr));
    nl_attr(b, IFA_ADDRESS, &address_network_order, sizeof(address_network_order));
    nl_attr(b, IFA_LOCAL, &address_network_order, sizeof(address_network_order));
    nl_attr(b, IFA_LABEL, label, strlen(label) + 1);
    nl_msg_end(b, header);
}

static int dump_addrs(struct nl_builder *b, uint32_t seq, uint32_t pid) {
    uint32_t loopback = net_htonl(0x7F000001U);
    emit_addr(b, seq, pid, NETLINK_INDEX_LO, "lo", 8, 254, loopback);

    const struct net_config *config = net_get_config();
    if (config->address) {
        emit_addr(b, seq, pid, NETLINK_INDEX_ETH0, "eth0",
                  netmask_prefix(config->netmask), RT_SCOPE_UNIVERSE, config->address);
    }
    return 1;
}

static void emit_route(struct nl_builder *b, uint32_t seq, uint32_t pid, uint8_t dst_len,
                       const uint32_t *dst, const uint32_t *gateway, const uint32_t *prefsrc,
                       int oif, uint8_t scope, uint8_t protocol) {
    struct rtmsg route;
    memset(&route, 0, sizeof(route));
    route.rtm_family = NL_AF_INET;
    route.rtm_dst_len = dst_len;
    route.rtm_table = RT_TABLE_MAIN;
    route.rtm_protocol = protocol;
    route.rtm_scope = scope;
    route.rtm_type = RTN_UNICAST;
    struct nlmsghdr *header = nl_msg_begin(b, RTM_NEWROUTE, NLM_F_MULTI, seq, pid,
                                           &route, sizeof(route));
    nl_attr_u32(b, RTA_TABLE, RT_TABLE_MAIN);
    if (dst) nl_attr(b, RTA_DST, dst, sizeof(*dst));
    if (prefsrc) nl_attr(b, RTA_PREFSRC, prefsrc, sizeof(*prefsrc));
    if (gateway) nl_attr(b, RTA_GATEWAY, gateway, sizeof(*gateway));
    nl_attr_u32(b, RTA_OIF, (uint32_t)oif);
    nl_msg_end(b, header);
}

static int dump_routes(struct nl_builder *b, uint32_t seq, uint32_t pid) {
    const struct net_config *config = net_get_config();
    if (config->address && config->netmask) {
        /* Emit the connected route. */
        uint32_t network = config->address & config->netmask;
        emit_route(b, seq, pid, netmask_prefix(config->netmask), &network, NULL,
                   &config->address, NETLINK_INDEX_ETH0, RT_SCOPE_LINK, RTPROT_KERNEL);
    }
    if (config->gateway) {
        /* Emit the default route. */
        emit_route(b, seq, pid, 0, NULL, &config->gateway, NULL,
                   NETLINK_INDEX_ETH0, RT_SCOPE_UNIVERSE, RTPROT_BOOT);
    }
    return 1;
}

/* Dispatch netlink requests. */

static const void *request_attr(const struct nlmsghdr *request, size_t header_length,
                                uint16_t wanted, size_t *value_length) {
    if (request->nlmsg_len < sizeof(*request) + header_length) return NULL;
    const uint8_t *base = (const uint8_t *)request;
    size_t offset = NLMSG_ALIGN(sizeof(*request) + header_length);
    while (offset + sizeof(struct rtattr) <= request->nlmsg_len) {
        const struct rtattr *attr = (const struct rtattr *)(base + offset);
        if (attr->rta_len < sizeof(*attr) || offset + attr->rta_len > request->nlmsg_len)
            break;
        if (attr->rta_type == wanted) {
            if (value_length) *value_length = attr->rta_len - sizeof(*attr);
            return base + offset + sizeof(*attr);
        }
        offset += NLMSG_ALIGN(attr->rta_len);
    }
    return NULL;
}

static int handle_addr_change(const struct nlmsghdr *request) {
    if (request->nlmsg_len < sizeof(*request) + sizeof(struct ifaddrmsg)) return -EINVAL;
    const struct ifaddrmsg *address =
        (const struct ifaddrmsg *)((const uint8_t *)request + sizeof(*request));
    if (address->ifa_family != NL_AF_INET || address->ifa_index != NETLINK_INDEX_ETH0)
        return -ENODEV;
    size_t value_length = 0;
    const uint32_t *value = request_attr(request, sizeof(*address), IFA_LOCAL, &value_length);
    if (!value) value = request_attr(request, sizeof(*address), IFA_ADDRESS, &value_length);
    if (request->nlmsg_type == RTM_DELADDR) {
        net_set_address(0);
        net_set_netmask(0);
        return 0;
    }
    if (!value || value_length < sizeof(*value) || address->ifa_prefixlen > 32U)
        return -EINVAL;
    uint32_t mask = address->ifa_prefixlen
        ? 0xFFFFFFFFU << (32U - address->ifa_prefixlen) : 0U;
    net_set_address(*value);
    net_set_netmask(net_htonl(mask));
    return 0;
}

static int handle_route_change(const struct nlmsghdr *request) {
    if (request->nlmsg_len < sizeof(*request) + sizeof(struct rtmsg)) return -EINVAL;
    const struct rtmsg *route =
        (const struct rtmsg *)((const uint8_t *)request + sizeof(*request));
    if (route->rtm_family != NL_AF_INET) return -EINVAL;
    size_t value_length = 0;
    const uint32_t *gateway =
        request_attr(request, sizeof(*route), RTA_GATEWAY, &value_length);
    if (request->nlmsg_type == RTM_DELROUTE) {
        if (route->rtm_dst_len == 0U) net_set_gateway(0);
        return 0;
    }
    if (gateway && value_length >= sizeof(*gateway)) net_set_gateway(*gateway);
    return 0;
}

static int handle_route_request(struct nl_builder *b, const struct nlmsghdr *request,
                                uint32_t portid) {
    uint32_t seq = request->nlmsg_seq;
    uint32_t pid = portid;
    switch (request->nlmsg_type) {
        case RTM_GETLINK: return dump_links(b, seq, pid, request);
        case RTM_GETADDR: return dump_addrs(b, seq, pid);
        case RTM_GETROUTE: return dump_routes(b, seq, pid);
        case RTM_NEWADDR:
        case RTM_DELADDR:
            nl_put_error(b, seq, pid, request, handle_addr_change(request));
            return 0;
        case RTM_NEWROUTE:
        case RTM_DELROUTE:
            nl_put_error(b, seq, pid, request, handle_route_change(request));
            return 0;
        default:
            if (request->nlmsg_flags & NLM_F_ACK)
                nl_put_error(b, seq, pid, request, 0);
            else
                nl_put_error(b, seq, pid, request, -EOPNOTSUPP);
            return 0;
    }
}

static int handle_diag_request(struct nl_builder *b, const struct nlmsghdr *request,
                               uint32_t portid) {
    /* Return an empty socket dump. */
    (void)b;
    (void)request;
    (void)portid;
    return 1;
}

static int handle_generic_request(struct nl_builder *b, const struct nlmsghdr *request,
                                  uint32_t portid) {
    /* Report unavailable generic families. */
    nl_put_error(b, request->nlmsg_seq, portid, request, -ENOENT);
    return 0;
}

/* Queue one nonempty datagram. */
static int nl_rx_queue_from(struct netlink_socket *socket, const uint8_t *data,
                            size_t length, uint32_t source_portid,
                            uint32_t source_groups,
                            const struct netlink_credentials *credentials) {
    if (!length) return 0;
    struct netlink_datagram *datagram =
        (struct netlink_datagram *)kmalloc(sizeof(*datagram) + length);
    if (!datagram) return -1;
    datagram->next = NULL;
    datagram->source_portid = source_portid;
    datagram->source_groups = source_groups;
    if (credentials) datagram->source_credentials = *credentials;
    else memset(&datagram->source_credentials, 0, sizeof(datagram->source_credentials));
    datagram->length = length;
    memcpy(datagram->data, data, length);
    if (socket->rx_tail) socket->rx_tail->next = datagram;
    else socket->rx_head = datagram;
    socket->rx_tail = datagram;
    return 0;
}

/* Queue a kernel reply. */
static int nl_rx_queue(struct netlink_socket *socket, const uint8_t *data, size_t length) {
    return nl_rx_queue_from(socket, data, length, 0, 0, NULL);
}

/* Multicast a uevent. */
static void netlink_uevent_multicast(uint32_t groups, const void *data, size_t length,
                                     uint32_t source_portid,
                                     const struct netlink_credentials *credentials,
                                     const struct netlink_socket *sender) {
    if (!groups || !length) return;
    for (struct netlink_socket *socket = netlink_registry; socket;
         socket = socket->registry_next) {
        if (socket == sender) continue;
        if (socket->protocol != TUNIX_NETLINK_KOBJECT_UEVENT) continue;
        if (!(socket->groups & groups)) continue;
        (void)nl_rx_queue_from(socket, (const uint8_t *)data, length, source_portid,
                               groups, credentials);
    }
}

/* Unicast a uevent. */
static void netlink_uevent_unicast(uint32_t portid, const void *data, size_t length,
                                   uint32_t source_portid,
                                   const struct netlink_credentials *credentials,
                                   const struct netlink_socket *sender) {
    if (!portid || !length) return;
    for (struct netlink_socket *socket = netlink_registry; socket;
         socket = socket->registry_next) {
        if (socket == sender) continue;
        if (socket->protocol != TUNIX_NETLINK_KOBJECT_UEVENT) continue;
        if (socket->portid != portid) continue;
        (void)nl_rx_queue_from(socket, (const uint8_t *)data, length, source_portid,
                               0, credentials);
        return;
    }
}

void netlink_uevent_broadcast(const void *message, size_t length) {
    /* Broadcast as the kernel. */
    struct netlink_credentials kernel = {0, 0, 0};
    netlink_uevent_multicast(1U << (TUNIX_UEVENT_GROUP_KERNEL - 1), message, length,
                             0, &kernel, NULL);
}

int64_t netlink_socket_sendto(struct netlink_socket *socket, const void *data, size_t length,
                              int flags, const void *address, size_t address_length) {
    (void)flags;
    (void)address;
    (void)address_length;
    if (!socket) return -EINVAL;
    uint32_t portid = netlink_assign_portid(socket);

    /* Route uevent datagrams. */
    if (socket->protocol == TUNIX_NETLINK_KOBJECT_UEVENT) {
        const struct tunix_sockaddr_nl *destination =
            (const struct tunix_sockaddr_nl *)address;
        if (destination && address_length >= sizeof(*destination)) {
            struct netlink_credentials sender;
            const struct credentials *self = cred_current();
            sender.pid = (uint32_t)process_current_pid();
            sender.uid = self ? self->euid : 0U;
            sender.gid = self ? self->egid : 0U;
            if (destination->groups)
                netlink_uevent_multicast(destination->groups, data, length, portid,
                                         &sender, socket);
            else
                netlink_uevent_unicast(destination->pid, data, length, portid,
                                       &sender, socket);
        }
        return (int64_t)length;
    }

    size_t cap = 8192;
    struct nl_builder builder = {0};
    builder.buf = (uint8_t *)kmalloc(cap);
    if (!builder.buf) return -EINVAL;
    builder.cap = cap;

    const uint8_t *bytes = (const uint8_t *)data;
    size_t offset = 0;
    while (offset + sizeof(struct nlmsghdr) <= length) {
        const struct nlmsghdr *request = (const struct nlmsghdr *)(bytes + offset);
        if (request->nlmsg_len < sizeof(struct nlmsghdr) ||
            offset + request->nlmsg_len > length) break;
        int dump;
        if (socket->protocol == TUNIX_NETLINK_ROUTE)
            dump = handle_route_request(&builder, request, portid);
        else if (socket->protocol == NETLINK_GENERIC)
            dump = handle_generic_request(&builder, request, portid);
        else
            dump = handle_diag_request(&builder, request, portid);

        /* Queue dump terminators separately. */
        nl_rx_queue(socket, builder.buf, builder.len);
        builder.len = 0;
        builder.overflow = 0;
        if (dump) {
            nl_put_done(&builder, request->nlmsg_seq, portid);
            nl_rx_queue(socket, builder.buf, builder.len);
            builder.len = 0;
            builder.overflow = 0;
        }
        offset += NLMSG_ALIGN(request->nlmsg_len);
    }

    kfree(builder.buf);
    return (int64_t)length;
}

int64_t netlink_socket_recvfrom(struct netlink_socket *socket, void *data, size_t length,
                                int flags, void *address, size_t *address_length) {
    if (!socket) return -EINVAL;
    struct netlink_datagram *datagram = socket->rx_head;
    if (!datagram) return -EAGAIN;
    size_t available = datagram->length;

    /* Preserve datagram read semantics. */
    size_t copy = available < length ? available : length;
    if (copy) memcpy(data, datagram->data, copy);

    uint32_t source_portid = datagram->source_portid;
    uint32_t source_groups = datagram->source_groups;
    socket->last_credentials = datagram->source_credentials;

    if (!(flags & NL_MSG_PEEK)) {
        socket->rx_head = datagram->next;
        if (!socket->rx_head) socket->rx_tail = NULL;
        kfree(datagram);
    }

    if (address && address_length) {
        struct tunix_sockaddr_nl nl;
        memset(&nl, 0, sizeof(nl));
        nl.family = TUNIX_AF_NETLINK;
        /* Preserve source identity. */
        nl.pid = source_portid;
        nl.groups = source_groups;
        size_t addr_copy = *address_length < sizeof(nl) ? *address_length : sizeof(nl);
        memcpy(address, &nl, addr_copy);
        *address_length = sizeof(nl);
    }
    return (flags & NL_MSG_TRUNC) ? (int64_t)available : (int64_t)copy;
}

int64_t netlink_socket_read(struct netlink_socket *socket, size_t length, void *data) {
    return netlink_socket_recvfrom(socket, data, length, 0, NULL, NULL);
}

int64_t netlink_socket_write(struct netlink_socket *socket, size_t length, const void *data) {
    return netlink_socket_sendto(socket, data, length, 0, NULL, 0);
}

int netlink_socket_read_ready(struct netlink_socket *socket) {
    return socket && socket->rx_head != NULL;
}

int netlink_socket_write_ready(struct netlink_socket *socket) {
    (void)socket;
    return 1;
}
