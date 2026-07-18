/* Minimal AF_NETLINK implementation for Tunix.

   Scope: everything iproute2's `ip` and `ss` need to run against Tunix's
   single-interface network model, and nothing more.  A netlink socket here is
   a synchronous request/response message queue -- when userspace writes an
   rtnetlink request we synthesize the whole reply (all NLMSG entries plus the
   terminating NLMSG_DONE) into the socket's receive buffer, and subsequent
   reads drain it a whole-message at a time.  There is no multicast, no async
   notification, and no blocking: a dump is fully materialized before send()
   returns, so the fd is immediately readable.

   The interface/address/route data is projected from net_get_config(): a fixed
   loopback plus the one eth0 the rtl8139 driver backs. */

#include <stddef.h>
#include <stdint.h>
#include "../include/heap.h"
#include "../include/kstring.h"
#include "../include/net/netlink.h"
#include "../include/net/net.h"

#define EAGAIN 11
#define EINVAL 22
#define EOPNOTSUPP 95

#define NL_MSG_PEEK 0x2
#define NL_MSG_TRUNC 0x20

/* ---- netlink / rtnetlink wire constants (kernel side) ------------------- */

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
#define RTM_GETADDR 22
#define RTM_NEWROUTE 24
#define RTM_GETROUTE 26

#define IFLA_ADDRESS 1
#define IFLA_BROADCAST 2
#define IFLA_IFNAME 3
#define IFLA_MTU 4

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

#define NL_AF_UNSPEC 0
#define NL_AF_INET 2

#define RT_TABLE_MAIN 254
#define RT_SCOPE_UNIVERSE 0
#define RT_SCOPE_LINK 253
#define RTPROT_BOOT 3
#define RTPROT_KERNEL 2
#define RTN_UNICAST 1

#define NETLINK_INDEX_LO 1
#define NETLINK_INDEX_ETH0 2

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

/* ---- netlink socket object ---------------------------------------------- */

struct netlink_socket {
    int refs;
    int protocol;
    uint32_t portid;
    int bound;
    uint8_t *rx;
    size_t rx_len;
    size_t rx_pos;
};

static uint32_t netlink_next_portid = 0;

struct netlink_socket *netlink_socket_create(int protocol) {
    if (protocol != TUNIX_NETLINK_ROUTE && protocol != TUNIX_NETLINK_SOCK_DIAG) return NULL;
    struct netlink_socket *socket = (struct netlink_socket *)kmalloc(sizeof(*socket));
    if (!socket) return NULL;
    memset(socket, 0, sizeof(*socket));
    socket->refs = 1;
    socket->protocol = protocol;
    return socket;
}

void netlink_socket_ref(struct netlink_socket *socket) {
    if (socket) socket->refs++;
}

void netlink_socket_unref(struct netlink_socket *socket) {
    if (!socket || socket->refs <= 0) return;
    if (--socket->refs != 0) return;
    if (socket->rx) kfree(socket->rx);
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
    socket->bound = 1;
    return 0;
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

/* ---- response builder --------------------------------------------------- */

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
    b->len += total;
}

static void nl_msg_end(struct nl_builder *b, struct nlmsghdr *header) {
    if (!header || b->overflow) return;
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

/* ---- interface projection ----------------------------------------------- */

static uint8_t netmask_prefix(uint32_t netmask_network_order) {
    /* netmask bytes are in network order in memory; count the set bits. */
    uint8_t prefix = 0;
    const uint8_t *bytes = (const uint8_t *)&netmask_network_order;
    for (int i = 0; i < 4; i++) {
        uint8_t byte = bytes[i];
        while (byte & 0x80U) { prefix++; byte = (uint8_t)(byte << 1); }
    }
    return prefix;
}

static void emit_link(struct nl_builder *b, uint32_t seq, uint32_t pid, int index,
                      const char *name, uint16_t arptype, uint32_t flags, uint32_t mtu,
                      const uint8_t *mac, int mac_length) {
    struct ifinfomsg info;
    memset(&info, 0, sizeof(info));
    info.ifi_family = NL_AF_UNSPEC;
    info.ifi_type = arptype;
    info.ifi_index = index;
    info.ifi_flags = flags;
    struct nlmsghdr *header = nl_msg_begin(b, RTM_NEWLINK, NLM_F_MULTI, seq, pid,
                                           &info, sizeof(info));
    nl_attr(b, IFLA_IFNAME, name, strlen(name) + 1);
    nl_attr(b, IFLA_MTU, &mtu, sizeof(mtu));
    if (mac_length) {
        nl_attr(b, IFLA_ADDRESS, mac, (size_t)mac_length);
        uint8_t broadcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
        nl_attr(b, IFLA_BROADCAST, broadcast, sizeof(broadcast));
    }
    nl_msg_end(b, header);
}

static void dump_links(struct nl_builder *b, uint32_t seq, uint32_t pid) {
    uint8_t loopback_mac[6] = {0, 0, 0, 0, 0, 0};
    emit_link(b, seq, pid, NETLINK_INDEX_LO, "lo", ARPHRD_LOOPBACK,
              IFF_UP | IFF_LOOPBACK | IFF_RUNNING, 65536U, loopback_mac, 0);

    const struct net_config *config = net_get_config();
    uint32_t flags = IFF_BROADCAST | IFF_MULTICAST;
    if (config->interface_up) flags |= IFF_UP;
    if (config->link_up) flags |= IFF_RUNNING;
    emit_link(b, seq, pid, NETLINK_INDEX_ETH0, "eth0", ARPHRD_ETHER, flags, 1500U,
              config->mac, 6);
    nl_put_done(b, seq, pid);
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

static void dump_addrs(struct nl_builder *b, uint32_t seq, uint32_t pid) {
    uint32_t loopback = net_htonl(0x7F000001U); /* 127.0.0.1 */
    emit_addr(b, seq, pid, NETLINK_INDEX_LO, "lo", 8, 254 /* RT_SCOPE_HOST */, loopback);

    const struct net_config *config = net_get_config();
    if (config->address) {
        emit_addr(b, seq, pid, NETLINK_INDEX_ETH0, "eth0",
                  netmask_prefix(config->netmask), RT_SCOPE_UNIVERSE, config->address);
    }
    nl_put_done(b, seq, pid);
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

static void dump_routes(struct nl_builder *b, uint32_t seq, uint32_t pid) {
    const struct net_config *config = net_get_config();
    if (config->address && config->netmask) {
        /* on-link subnet route: <network>/<prefix> dev eth0 proto kernel scope link */
        uint32_t network = config->address & config->netmask;
        emit_route(b, seq, pid, netmask_prefix(config->netmask), &network, NULL,
                   &config->address, NETLINK_INDEX_ETH0, RT_SCOPE_LINK, RTPROT_KERNEL);
    }
    if (config->gateway) {
        /* default route via gateway */
        emit_route(b, seq, pid, 0, NULL, &config->gateway, NULL,
                   NETLINK_INDEX_ETH0, RT_SCOPE_UNIVERSE, RTPROT_BOOT);
    }
    nl_put_done(b, seq, pid);
}

/* ---- request dispatch --------------------------------------------------- */

static void handle_route_request(struct nl_builder *b, const struct nlmsghdr *request) {
    uint32_t seq = request->nlmsg_seq;
    uint32_t pid = request->nlmsg_pid;
    switch (request->nlmsg_type) {
        case RTM_GETLINK: dump_links(b, seq, pid); break;
        case RTM_GETADDR: dump_addrs(b, seq, pid); break;
        case RTM_GETROUTE: dump_routes(b, seq, pid); break;
        default:
            if (request->nlmsg_flags & NLM_F_ACK)
                nl_put_error(b, seq, pid, request, 0);
            else
                nl_put_error(b, seq, pid, request, -EOPNOTSUPP);
            break;
    }
}

static void handle_diag_request(struct nl_builder *b, const struct nlmsghdr *request) {
    /* ss issues SOCK_DIAG_BY_FAMILY dumps. We have no socket-table enumeration
       wired in yet, so answer every dump with an empty result (NLMSG_DONE):
       ss then prints just its header rather than failing on the socket. */
    nl_put_done(b, request->nlmsg_seq, request->nlmsg_pid);
}

static int nl_rx_append(struct netlink_socket *socket, const uint8_t *data, size_t length) {
    size_t live = socket->rx_len - socket->rx_pos;
    size_t total = live + length;
    uint8_t *buffer = (uint8_t *)kmalloc(total ? total : 1);
    if (!buffer) return -1;
    if (live) memcpy(buffer, socket->rx + socket->rx_pos, live);
    if (length) memcpy(buffer + live, data, length);
    if (socket->rx) kfree(socket->rx);
    socket->rx = buffer;
    socket->rx_len = total;
    socket->rx_pos = 0;
    return 0;
}

int64_t netlink_socket_sendto(struct netlink_socket *socket, const void *data, size_t length,
                              int flags, const void *address, size_t address_length) {
    (void)flags;
    (void)address;
    (void)address_length;
    if (!socket) return -EINVAL;
    netlink_assign_portid(socket);

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
        if (socket->protocol == TUNIX_NETLINK_ROUTE)
            handle_route_request(&builder, request);
        else
            handle_diag_request(&builder, request);
        offset += NLMSG_ALIGN(request->nlmsg_len);
    }

    if (builder.len) nl_rx_append(socket, builder.buf, builder.len);
    kfree(builder.buf);
    return (int64_t)length;
}

int64_t netlink_socket_recvfrom(struct netlink_socket *socket, void *data, size_t length,
                                int flags, void *address, size_t *address_length) {
    if (!socket) return -EINVAL;
    size_t available = socket->rx_len - socket->rx_pos;
    if (available == 0) return -EAGAIN;

    /* The whole synthesized reply is treated as one netlink datagram, matching
       how iproute2's libnetlink drives us: it first probes the size with
       MSG_PEEK|MSG_TRUNC (a zero-length buffer that must still report the full
       length without consuming), allocates exactly that, then reads for real.
       So a MSG_TRUNC read reports the real length even when it does not fit,
       and only a non-peek read advances the queue. */
    const uint8_t *base = socket->rx + socket->rx_pos;
    size_t copy = available < length ? available : length;
    if (copy) memcpy(data, base, copy);

    if (!(flags & NL_MSG_PEEK)) {
        size_t consumed = (flags & NL_MSG_TRUNC) ? available : copy;
        socket->rx_pos += consumed;
    }

    if (address && address_length) {
        struct tunix_sockaddr_nl nl;
        memset(&nl, 0, sizeof(nl));
        nl.family = TUNIX_AF_NETLINK;
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
    return socket && socket->rx_len > socket->rx_pos;
}

int netlink_socket_write_ready(struct netlink_socket *socket) {
    (void)socket;
    return 1;
}
