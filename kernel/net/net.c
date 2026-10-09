#include <stddef.h>
#include <stdint.h>
#include <tunix/boot.h>
#include <tunix/cpu.h>
#include <tunix/heap.h>
#include <tunix/kstring.h>
#include <tunix/time.h>
#include <tunix/net/inet_socket.h>
#include <tunix/net/net.h>
#include <tunix/net/virtio_net.h>
#include <tunix/workqueue.h>

extern void kprintf(const char *fmt, ...);

#define ETHERTYPE_IPV4 0x0800U
#define ETHERTYPE_ARP 0x0806U
#define IPPROTO_ICMP 1U
#define IPPROTO_UDP 17U
#define ARP_CACHE_SIZE 256

struct ethernet_header {
    uint8_t destination[6];
    uint8_t source[6];
    uint16_t type;
} __attribute__((packed));

struct arp_packet {
    uint16_t hardware_type;
    uint16_t protocol_type;
    uint8_t hardware_length;
    uint8_t protocol_length;
    uint16_t operation;
    uint8_t sender_mac[6];
    uint32_t sender_ip;
    uint8_t target_mac[6];
    uint32_t target_ip;
} __attribute__((packed));

struct ipv4_header {
    uint8_t version_ihl;
    uint8_t tos;
    uint16_t total_length;
    uint16_t identification;
    uint16_t fragment;
    uint8_t ttl;
    uint8_t protocol;
    uint16_t checksum;
    uint32_t source;
    uint32_t destination;
} __attribute__((packed));

struct udp_header {
    uint16_t source_port;
    uint16_t destination_port;
    uint16_t length;
    uint16_t checksum;
} __attribute__((packed));

struct tcp_header {
    uint16_t source_port;
    uint16_t destination_port;
    uint32_t seq;
    uint32_t ack;
    uint8_t data_offset;
    uint8_t flags;
    uint16_t window;
    uint16_t checksum;
    uint16_t urgent;
} __attribute__((packed));

struct icmp_header {
    uint8_t type;
    uint8_t code;
    uint16_t checksum;
    uint32_t rest;
} __attribute__((packed));

struct arp_entry {
    uint32_t ip;
    uint8_t mac[6];
    uint64_t updated_ns;
};

#define LOOPBACK_BUDGET (16U << 20)
#define LOOPBACK_BURST 128
#define REASSEMBLY_LIMIT 64U
#define REASSEMBLY_TIMEOUT_NS 30000000000ULL
#define IPV4_MORE_FRAGMENTS 0x2000U
#define IPV4_DONT_FRAGMENT 0x4000U
#define IPV4_OFFSET_MASK 0x1FFFU

struct loopback_packet {
    struct loopback_packet *next;
    size_t length;
    uint8_t data[];
};

struct lock net_lock = LOCK_INITIALIZER("net", LOCK_RANK_NET);

static struct loopback_packet *loopback_first;
static struct loopback_packet *loopback_last;
static size_t loopback_bytes;
static uint64_t loopback_dropped;

struct reassembly {
    struct reassembly *next;
    uint32_t source;
    uint32_t destination;
    uint16_t identification;
    uint8_t protocol;
    uint8_t header_length;
    uint64_t deadline_ns;
    size_t total;
    size_t received;
    uint8_t header[60];
    uint8_t filled[NET_IPV4_MAX / 64U + 1U];
    uint8_t data[NET_IPV4_MAX];
};

static struct reassembly *reassemblies;
static unsigned reassembly_count;

static struct net_config config;
static struct arp_entry arp_cache[ARP_CACHE_SIZE];
static unsigned arp_replace;
static uint16_t ipv4_identification;
static uint64_t stack_rx;
static uint64_t stack_rx_bytes;
static uint64_t stack_tx;
static uint64_t stack_tx_bytes;
static uint64_t stack_drop;
static int interrupts_wanted;

static const struct net_adapter *adapter;

static int adapter_transmit(const void *frame, size_t length) {
    return adapter ? adapter->transmit(frame, length) : -1;
}

uint16_t net_htons(uint16_t value) { return (uint16_t)((value << 8) | (value >> 8)); }
uint32_t net_htonl(uint32_t value) {
    return ((value & 0x000000FFU) << 24) | ((value & 0x0000FF00U) << 8) |
           ((value & 0x00FF0000U) >> 8) | ((value & 0xFF000000U) >> 24);
}

uint16_t net_checksum(const void *data, size_t length) {
    const uint8_t *bytes = (const uint8_t *)data;
    uint32_t sum = 0;
    while (length >= 2U) {
        sum += ((uint16_t)bytes[0] << 8) | bytes[1];
        bytes += 2;
        length -= 2;
    }
    if (length) sum += (uint16_t)bytes[0] << 8;
    while (sum >> 16) sum = (sum & 0xFFFFU) + (sum >> 16);
    return (uint16_t)~sum;
}

int net_is_loopback(uint32_t address) {
    return (net_htonl(address) & NET_LOOPBACK_MASK) == NET_LOOPBACK_NETWORK;
}

static int address_is_local(uint32_t address) {
    return net_is_loopback(address) || (config.address && address == config.address);
}

uint32_t net_source_for(uint32_t destination) {
    NET_LOCKED;
    if (net_is_loopback(destination)) return net_htonl(NET_LOOPBACK_ADDRESS);
    return config.address;
}

static uint8_t *loopback_reserve(size_t length, struct loopback_packet **out) {
    if (!length || length > NET_IPV4_MAX || loopback_bytes + length > LOOPBACK_BUDGET) {
        loopback_dropped++;
        return NULL;
    }
    struct loopback_packet *slot =
        (struct loopback_packet *)kmalloc(sizeof(*slot) + length);
    if (!slot) {
        loopback_dropped++;
        return NULL;
    }
    slot->next = NULL;
    slot->length = length;
    *out = slot;
    return slot->data;
}

static void loopback_commit(struct loopback_packet *slot) {
    if (loopback_last) loopback_last->next = slot;
    else loopback_first = slot;
    loopback_last = slot;
    loopback_bytes += slot->length;
    stack_tx++;
    stack_tx_bytes += slot->length;
}

static int loopback_enqueue(const void *packet, size_t length) {
    struct loopback_packet *slot;
    uint8_t *data = loopback_reserve(length, &slot);
    if (!data) return -1;
    memcpy(data, packet, length);
    loopback_commit(slot);
    return 0;
}

size_t net_path_mtu(uint32_t destination) {
    NET_LOCKED;
    return address_is_local(destination) ? NET_LOOPBACK_MTU : NET_MTU;
}

static int mac_equal(const uint8_t *left, const uint8_t *right) {
    for (unsigned i = 0; i < 6; i++) if (left[i] != right[i]) return 0;
    return 1;
}

static void arp_learn(uint32_t ip, const uint8_t mac[6]) {
    if (!ip) return;
    for (unsigned i = 0; i < ARP_CACHE_SIZE; i++) {
        if (arp_cache[i].ip == ip) {
            memcpy(arp_cache[i].mac, mac, 6);
            arp_cache[i].updated_ns = time_uptime_ns();
            return;
        }
    }
    struct arp_entry *entry = &arp_cache[arp_replace++ % ARP_CACHE_SIZE];
    entry->ip = ip;
    memcpy(entry->mac, mac, 6);
    entry->updated_ns = time_uptime_ns();
}

static const uint8_t *arp_lookup(uint32_t ip) {
    for (unsigned i = 0; i < ARP_CACHE_SIZE; i++)
        if (arp_cache[i].ip == ip) return arp_cache[i].mac;
    return NULL;
}

int net_send_ethernet(const uint8_t destination[6], uint16_t type,
                         const void *payload, size_t length) {
    NET_LOCKED;
    if (!config.interface_up || length > NET_MTU) return -1;
    uint8_t frame[1514];
    struct ethernet_header *header = (struct ethernet_header *)frame;
    memcpy(header->destination, destination, 6);
    memcpy(header->source, config.mac, 6);
    header->type = net_htons(type);
    memcpy(frame + sizeof(*header), payload, length);
    if (adapter_transmit(frame, sizeof(*header) + length) != 0) return -1;
    stack_tx++;
    stack_tx_bytes += sizeof(*header) + length;
    return 0;
}

int net_send_raw_ethernet(const void *frame, size_t length) {
    NET_LOCKED;
    if (!config.interface_up || !frame || length < 14U || length > 1514U) return -1;
    if (adapter_transmit(frame, length) != 0) return -1;
    stack_tx++;
    stack_tx_bytes += length;
    return 0;
}

static void arp_send(uint16_t operation, const uint8_t destination_mac[6],
                     uint32_t target_ip, const uint8_t target_mac[6]) {
    struct arp_packet packet;
    packet.hardware_type = net_htons(1);
    packet.protocol_type = net_htons(ETHERTYPE_IPV4);
    packet.hardware_length = 6;
    packet.protocol_length = 4;
    packet.operation = net_htons(operation);
    memcpy(packet.sender_mac, config.mac, 6);
    packet.sender_ip = config.address;
    memcpy(packet.target_mac, target_mac, 6);
    packet.target_ip = target_ip;
    (void)net_send_ethernet(destination_mac, ETHERTYPE_ARP, &packet, sizeof(packet));
}

static const uint8_t *resolve_mac(uint32_t destination) {
    static const uint8_t broadcast[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
    if (destination == 0xFFFFFFFFU ||
        (config.netmask && (destination | config.netmask) == 0xFFFFFFFFU)) return broadcast;
    uint32_t next_hop = destination;
    if (config.gateway && config.netmask &&
        ((destination & config.netmask) != (config.address & config.netmask))) next_hop = config.gateway;
    const uint8_t *found = arp_lookup(next_hop);
    if (found) return found;
    uint8_t zero[6] = {0};
    arp_send(1, broadcast, next_hop, zero);
    uint64_t deadline = time_uptime_ns() + 1000000000ULL;
    while (time_uptime_ns() < deadline) {
        net_poll();
        found = arp_lookup(next_hop);
        if (found) return found;
        cpu_relax();
    }
    return NULL;
}

static void fill_ipv4_header(struct ipv4_header *header, uint32_t destination,
                             uint8_t protocol, size_t length, uint16_t identification,
                             uint16_t fragment, uint8_t ttl) {
    memset(header, 0, sizeof(*header));
    header->version_ihl = 0x45U;
    header->total_length = net_htons((uint16_t)(sizeof(*header) + length));
    header->identification = net_htons(identification);
    header->fragment = net_htons(fragment);
    header->ttl = ttl ? ttl : 64U;
    header->protocol = protocol;
    header->source = net_source_for(destination);
    header->destination = destination;
    header->checksum = net_htons(net_checksum(header, sizeof(*header)));
}

static int send_fragments(const uint8_t *mac, uint32_t destination, uint8_t protocol,
                          const uint8_t *payload, size_t length, uint16_t identification,
                          uint8_t ttl) {
    size_t chunk = (NET_MTU - sizeof(struct ipv4_header)) & ~7U;
    uint8_t packet[NET_MTU];
    for (size_t offset = 0; offset < length; offset += chunk) {
        size_t part = length - offset < chunk ? length - offset : chunk;
        uint16_t fragment = (uint16_t)(offset / 8U);
        if (offset + part < length) fragment |= IPV4_MORE_FRAGMENTS;
        struct ipv4_header *header = (struct ipv4_header *)packet;
        fill_ipv4_header(header, destination, protocol, part, identification, fragment, ttl);
        memcpy(packet + sizeof(*header), payload + offset, part);
        if (net_send_ethernet(mac, ETHERTYPE_IPV4, packet, sizeof(*header) + part) != 0)
            return -1;
    }
    return 0;
}

int net_send_ipv4(uint32_t destination, uint8_t protocol, const void *payload, size_t length,
                  uint8_t ttl, int header_included) {
    NET_LOCKED;
    if (!payload) return -1;
    if (header_included) {
        if (length < sizeof(struct ipv4_header) || length > NET_IPV4_MAX) return -1;
        const struct ipv4_header *provided = (const struct ipv4_header *)payload;
        if (address_is_local(provided->destination))
            return loopback_enqueue(payload, length);
        if (!config.interface_up || length > NET_MTU) return -1;
        const uint8_t *mac = resolve_mac(provided->destination);
        return mac ? net_send_ethernet(mac, ETHERTYPE_IPV4, payload, length) : -1;
    }
    if (length > NET_IPV4_MAX - sizeof(struct ipv4_header)) return -1;
    uint16_t identification = ++ipv4_identification;
    size_t total = sizeof(struct ipv4_header) + length;
    if (address_is_local(destination)) {
        struct loopback_packet *slot;
        uint8_t *packet = loopback_reserve(total, &slot);
        if (!packet) return -1;
        fill_ipv4_header((struct ipv4_header *)packet, destination, protocol, length,
                         identification, IPV4_DONT_FRAGMENT, ttl);
        memcpy(packet + sizeof(struct ipv4_header), payload, length);
        loopback_commit(slot);
        return 0;
    }
    if (!config.interface_up) return -1;
    const uint8_t *mac = resolve_mac(destination);
    if (!mac) return -1;
    if (total > NET_MTU) {
        if (protocol == IPPROTO_TCP) return -1;
        return send_fragments(mac, destination, protocol, (const uint8_t *)payload, length,
                              identification, ttl);
    }
    uint8_t packet[NET_MTU];
    fill_ipv4_header((struct ipv4_header *)packet, destination, protocol, length,
                     identification, IPV4_DONT_FRAGMENT, ttl);
    memcpy(packet + sizeof(struct ipv4_header), payload, length);
    return net_send_ethernet(mac, ETHERTYPE_IPV4, packet, total);
}

static uint16_t udp_checksum(uint32_t source, uint32_t destination,
                             const void *udp, size_t length) {
    uint32_t sum = 0;
    const uint8_t *s = (const uint8_t *)&source;
    const uint8_t *d = (const uint8_t *)&destination;
    for (unsigned i = 0; i < 4; i += 2) {
        sum += ((uint16_t)s[i] << 8) | s[i + 1];
        sum += ((uint16_t)d[i] << 8) | d[i + 1];
    }
    sum += 17U;
    sum += (uint16_t)length;
    const uint8_t *bytes = (const uint8_t *)udp;
    while (length >= 2) {
        sum += ((uint16_t)bytes[0] << 8) | bytes[1];
        bytes += 2; length -= 2;
    }
    if (length) sum += (uint16_t)bytes[0] << 8;
    while (sum >> 16) sum = (sum & 0xFFFFU) + (sum >> 16);
    uint16_t result = (uint16_t)~sum;
    return result ? result : 0xFFFFU;
}

int net_send_udp(uint32_t source, uint16_t source_port, uint32_t destination,
                 uint16_t destination_port, const void *payload, size_t length) {
    NET_LOCKED;
    (void)source;
    size_t total = sizeof(struct udp_header) + length;
    if (total > NET_IPV4_MAX - sizeof(struct ipv4_header)) return -1;
    uint8_t *packet = (uint8_t *)kmalloc(total);
    if (!packet) return -1;
    struct udp_header *header = (struct udp_header *)packet;
    header->source_port = net_htons(source_port);
    header->destination_port = net_htons(destination_port);
    header->length = net_htons((uint16_t)total);
    header->checksum = 0;
    memcpy(packet + sizeof(*header), payload, length);
    header->checksum = net_htons(udp_checksum(net_source_for(destination), destination,
                                              packet, total));
    int status = net_send_ipv4(destination, IPPROTO_UDP, packet, total, 64, 0);
    kfree(packet);
    return status;
}

static uint16_t tcp_checksum(uint32_t source, uint32_t destination,
                             const void *segment, size_t length) {
    uint32_t sum = 0;
    const uint8_t *s = (const uint8_t *)&source;
    const uint8_t *d = (const uint8_t *)&destination;
    for (unsigned i = 0; i < 4; i += 2) {
        sum += ((uint16_t)s[i] << 8) | s[i + 1];
        sum += ((uint16_t)d[i] << 8) | d[i + 1];
    }
    sum += IPPROTO_TCP;
    sum += (uint16_t)length;
    const uint8_t *bytes = (const uint8_t *)segment;
    size_t remaining = length;
    while (remaining >= 2) {
        sum += ((uint16_t)bytes[0] << 8) | bytes[1];
        bytes += 2; remaining -= 2;
    }
    if (remaining) sum += (uint16_t)bytes[0] << 8;
    while (sum >> 16) sum = (sum & 0xFFFFU) + (sum >> 16);
    return (uint16_t)~sum;
}

static size_t put_tcp_options(uint8_t *out, const struct net_tcp_options *options) {
    size_t length = 0;
    if (!options) return 0;
    if (options->has_mss) {
        out[length++] = 2;
        out[length++] = 4;
        out[length++] = (uint8_t)(options->mss >> 8);
        out[length++] = (uint8_t)options->mss;
    }
    if (options->has_window_scale) {
        out[length++] = 1;
        out[length++] = 3;
        out[length++] = 3;
        out[length++] = options->window_scale;
    }
    return length;
}

int net_send_tcp(uint32_t source, uint16_t source_port, uint32_t destination,
                 uint16_t destination_port, uint32_t seq, uint32_t ack, uint8_t flags,
                 uint16_t window, const struct net_tcp_options *options,
                 const void *payload, size_t length) {
    NET_LOCKED;
    (void)source;
    uint8_t option_bytes[8];
    size_t option_length = put_tcp_options(option_bytes, options);
    size_t header_length = sizeof(struct tcp_header) + option_length;
    size_t total = header_length + length;
    if (total > net_path_mtu(destination) - sizeof(struct ipv4_header) ||
        total > NET_IPV4_MAX - sizeof(struct ipv4_header)) return -1;
    uint8_t small[NET_MTU];
    uint8_t *packet = total <= sizeof(small) ? small : (uint8_t *)kmalloc(total);
    if (!packet) return -1;
    struct tcp_header *header = (struct tcp_header *)packet;
    memset(header, 0, sizeof(*header));
    header->source_port = net_htons(source_port);
    header->destination_port = net_htons(destination_port);
    header->seq = net_htonl(seq);
    header->ack = net_htonl(ack);
    header->data_offset = (uint8_t)((header_length / 4U) << 4);
    header->flags = flags;
    header->window = net_htons(window);
    memcpy(packet + sizeof(*header), option_bytes, option_length);
    if (length) memcpy(packet + header_length, payload, length);
    header->checksum = net_htons(tcp_checksum(net_source_for(destination), destination,
                                              packet, total));
    int status = net_send_ipv4(destination, IPPROTO_TCP, packet, total, 64, 0);
    if (packet != small) kfree(packet);
    return status;
}

static void parse_tcp_options(const uint8_t *at, size_t length,
                              struct net_tcp_options *options) {
    memset(options, 0, sizeof(*options));
    size_t index = 0;
    while (index < length) {
        uint8_t kind = at[index];
        if (kind == 0) break;
        if (kind == 1) {
            index++;
            continue;
        }
        if (index + 1U >= length) break;
        uint8_t size = at[index + 1U];
        if (size < 2U || index + size > length) break;
        if (kind == 2 && size == 4U) {
            options->mss = (uint16_t)((at[index + 2U] << 8) | at[index + 3U]);
            options->has_mss = 1;
        } else if (kind == 3 && size == 3U) {
            options->window_scale = at[index + 2U] > 14U ? 14U : at[index + 2U];
            options->has_window_scale = 1;
        }
        index += size;
    }
}

static void handle_arp(const uint8_t *data, size_t length) {
    if (length < sizeof(struct arp_packet)) return;
    const struct arp_packet *packet = (const struct arp_packet *)data;
    if (net_htons(packet->hardware_type) != 1 || net_htons(packet->protocol_type) != ETHERTYPE_IPV4 ||
        packet->hardware_length != 6 || packet->protocol_length != 4) return;
    arp_learn(packet->sender_ip, packet->sender_mac);
    uint16_t operation = net_htons(packet->operation);
    if (operation == 1 && config.address && packet->target_ip == config.address)
        arp_send(2, packet->sender_mac, packet->sender_ip, packet->sender_mac);
}

static int address_accept(uint32_t destination) {
    if (!destination || destination == 0xFFFFFFFFU) return 1;
    if (net_is_loopback(destination)) return 1;
    if (destination == config.address) return 1;
    if (config.netmask && destination == (config.address | ~config.netmask)) return 1;
    return 0;
}

static void handle_icmp(const struct ipv4_header *ip, const uint8_t *data, size_t length) {
    if (length < sizeof(struct icmp_header)) return;
    const struct icmp_header *icmp = (const struct icmp_header *)data;
    if (net_checksum(data, length) != 0) return;
    inet_socket_receive_ipv4((const uint8_t *)ip, (size_t)net_htons(ip->total_length),
                             IPPROTO_ICMP, ip->source, ip->destination);
    if (icmp->type == 8 && icmp->code == 0 && address_is_local(ip->destination)) {
        uint8_t *reply = (uint8_t *)kmalloc(length);
        if (!reply) return;
        memcpy(reply, data, length);
        struct icmp_header *response = (struct icmp_header *)reply;
        response->type = 0;
        response->checksum = 0;
        response->checksum = net_htons(net_checksum(reply, length));
        (void)net_send_ipv4(ip->source, IPPROTO_ICMP, reply, length, 64, 0);
        kfree(reply);
    }
}

static void handle_ipv4(const uint8_t *data, size_t length);

static void reassembly_drop(struct reassembly **link) {
    struct reassembly *entry = *link;
    *link = entry->next;
    kfree(entry);
    reassembly_count--;
}

static void reassembly_expire(uint64_t now) {
    struct reassembly **link = &reassemblies;
    while (*link) {
        if ((*link)->deadline_ns <= now) {
            stack_drop++;
            reassembly_drop(link);
        } else {
            link = &(*link)->next;
        }
    }
}

static struct reassembly *reassembly_find(const struct ipv4_header *ip) {
    uint64_t now = time_uptime_ns();
    reassembly_expire(now);
    uint16_t identification = net_htons(ip->identification);
    for (struct reassembly *entry = reassemblies; entry; entry = entry->next)
        if (entry->source == ip->source && entry->destination == ip->destination &&
            entry->identification == identification && entry->protocol == ip->protocol)
            return entry;
    if (reassembly_count >= REASSEMBLY_LIMIT) {
        struct reassembly **oldest = &reassemblies;
        while ((*oldest)->next) oldest = &(*oldest)->next;
        stack_drop++;
        reassembly_drop(oldest);
    }
    struct reassembly *entry = (struct reassembly *)kmalloc(sizeof(*entry));
    if (!entry) return NULL;
    memset(entry, 0, offsetof(struct reassembly, data));
    entry->source = ip->source;
    entry->destination = ip->destination;
    entry->identification = identification;
    entry->protocol = ip->protocol;
    entry->deadline_ns = now + REASSEMBLY_TIMEOUT_NS;
    entry->next = reassemblies;
    reassemblies = entry;
    reassembly_count++;
    return entry;
}

static void reassemble(const struct ipv4_header *ip, size_t header_length,
                       const uint8_t *payload, size_t length) {
    uint16_t fragment = net_htons(ip->fragment);
    size_t offset = (size_t)(fragment & IPV4_OFFSET_MASK) * 8U;
    int last = !(fragment & IPV4_MORE_FRAGMENTS);
    if ((!last && (length & 7U)) || !length ||
        offset + length + header_length > NET_IPV4_MAX) {
        stack_drop++;
        return;
    }
    struct reassembly *entry = reassembly_find(ip);
    if (!entry) return;
    memcpy(entry->data + offset, payload, length);
    for (size_t unit = offset / 8U; unit * 8U < offset + length; unit++) {
        uint8_t bit = (uint8_t)(1U << (unit & 7U));
        if (entry->filled[unit / 8U] & bit) continue;
        entry->filled[unit / 8U] |= bit;
        size_t end = unit * 8U + 8U;
        entry->received += (end < offset + length ? end : offset + length) - unit * 8U;
    }
    if (last) entry->total = offset + length;
    if (!offset) {
        entry->header_length = (uint8_t)header_length;
        memcpy(entry->header, ip, header_length);
    }
    if (!entry->total || !entry->header_length || entry->received != entry->total) return;

    size_t whole = entry->header_length + entry->total;
    uint8_t *packet = (uint8_t *)kmalloc(whole);
    if (packet) {
        memcpy(packet, entry->header, entry->header_length);
        memcpy(packet + entry->header_length, entry->data, entry->total);
        struct ipv4_header *header = (struct ipv4_header *)packet;
        header->total_length = net_htons((uint16_t)whole);
        header->fragment = 0;
        header->checksum = 0;
        header->checksum = net_htons(net_checksum(header, entry->header_length));
    }
    for (struct reassembly **link = &reassemblies; *link; link = &(*link)->next) {
        if (*link != entry) continue;
        reassembly_drop(link);
        break;
    }
    if (!packet) return;
    handle_ipv4(packet, whole);
    kfree(packet);
}

static void handle_ipv4(const uint8_t *data, size_t length) {
    if (length < sizeof(struct ipv4_header)) return;
    const struct ipv4_header *ip = (const struct ipv4_header *)data;
    size_t header_length = (size_t)(ip->version_ihl & 0x0FU) * 4U;
    size_t total_length = net_htons(ip->total_length);
    if ((ip->version_ihl >> 4) != 4U || header_length < 20U || header_length > length ||
        total_length < header_length || total_length > length || net_checksum(data, header_length) != 0) {
        stack_drop++; return;
    }
    if (!address_accept(ip->destination)) return;
    if (net_htons(ip->fragment) & (IPV4_MORE_FRAGMENTS | IPV4_OFFSET_MASK)) {
        reassemble(ip, header_length, data + header_length, total_length - header_length);
        return;
    }
    const uint8_t *payload = data + header_length;
    size_t payload_length = total_length - header_length;
    if (ip->protocol == IPPROTO_ICMP) {
        handle_icmp(ip, payload, payload_length);
    } else if (ip->protocol == IPPROTO_UDP && payload_length >= sizeof(struct udp_header)) {
        const struct udp_header *udp = (const struct udp_header *)payload;
        size_t udp_length = net_htons(udp->length);
        if (udp_length < sizeof(*udp) || udp_length > payload_length) return;
        inet_socket_receive_udp(payload + sizeof(*udp), udp_length - sizeof(*udp), ip->source,
                                net_htons(udp->source_port), ip->destination,
                                net_htons(udp->destination_port));
    } else if (ip->protocol == IPPROTO_TCP && payload_length >= sizeof(struct tcp_header)) {
        const struct tcp_header *tcp = (const struct tcp_header *)payload;
        size_t data_offset = (size_t)((tcp->data_offset >> 4) & 0x0FU) * 4U;
        if (data_offset < sizeof(struct tcp_header) || data_offset > payload_length) {
            stack_drop++; return;
        }
        if (tcp_checksum(ip->source, ip->destination, payload, payload_length) != 0) {
            stack_drop++; return;
        }
        struct net_tcp_options options;
        parse_tcp_options(payload + sizeof(struct tcp_header),
                          data_offset - sizeof(struct tcp_header), &options);
        inet_socket_receive_tcp(ip->source, net_htons(tcp->source_port), ip->destination,
                                net_htons(tcp->destination_port), net_htonl(tcp->seq),
                                net_htonl(tcp->ack), tcp->flags, net_htons(tcp->window),
                                &options, payload + data_offset, payload_length - data_offset);
    } else {
        inet_socket_receive_ipv4(data, total_length, ip->protocol, ip->source, ip->destination);
    }
}

static void receive_frame(const uint8_t *frame, size_t length) {
    if (length < sizeof(struct ethernet_header)) { stack_drop++; return; }
    const struct ethernet_header *header = (const struct ethernet_header *)frame;
    static const uint8_t broadcast[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
    if (!mac_equal(header->destination, config.mac) && !mac_equal(header->destination, broadcast)) return;
    uint16_t type = net_htons(header->type);
    stack_rx++;
    stack_rx_bytes += length;
    inet_socket_receive_ethernet(frame, length, type);
    const uint8_t *payload = frame + sizeof(*header);
    size_t payload_length = length - sizeof(*header);
    if (type == ETHERTYPE_ARP) handle_arp(payload, payload_length);
    else if (type == ETHERTYPE_IPV4) handle_ipv4(payload, payload_length);
}

int net_register_adapter(const struct net_adapter *card) {
    NET_LOCKED;
    if (adapter || !card || !card->transmit || !card->poll) return -1;
    adapter = card;
    memcpy(config.mac, card->mac, sizeof(config.mac));
    config.link_up = 1;
    config.interface_up = 1;
    kprintf("NET: %s eth0 %x:%x:%x:%x:%x:%x ready\n", card->name,
            config.mac[0], config.mac[1], config.mac[2], config.mac[3],
            config.mac[4], config.mac[5]);
    if (interrupts_wanted) net_enable_interrupts();
    return 0;
}

void net_unregister_adapter(const struct net_adapter *card) {
    NET_LOCKED;
    if (!adapter || (card && card != adapter)) return;
    adapter = NULL;
    config.link_up = 0;
    config.interface_up = 0;
    memset(config.mac, 0, sizeof(config.mac));
}

void net_init(void) {
    memset(&config, 0, sizeof(config));
    memset(arp_cache, 0, sizeof(arp_cache));
    adapter = NULL;
    if (virtio_net_init() != 0) kprintf("NET: no built-in adapter found\n");
}

void net_enable_interrupts(void) {
    NET_LOCKED;
    interrupts_wanted = 1;
    if (boot_command_line_flag("nonetirq")) return;
    if (!adapter || !adapter->enable_interrupts) return;
    adapter->enable_interrupts();
    unsigned vector = adapter->interrupt_vector ? adapter->interrupt_vector() : 0;
    if (vector) kprintf("NET: %s interrupts on vector %u\n", adapter->name, vector);
}

int net_adapter_interrupts(void) {
    NET_LOCKED;
    if (!adapter || boot_command_line_flag("nonetirq")) return 0;
    if (!adapter->interrupt_vector) return 0;
    return adapter->interrupt_vector() != 0;
}

static void loopback_drain(void);
static void run_tcp_timers(void);

static void net_service(void *unused) {
    (void)unused;
    NET_LOCKED;
    loopback_drain();
    run_tcp_timers();
}

static struct work net_service_work = WORK_INITIALIZER(net_service, NULL);

void net_tick(void) {
    static unsigned beats;
    if (!lock_try_acquire(&net_lock)) return;
    if (adapter && !net_adapter_interrupts()) {
        net_poll();
    } else if (loopback_first || (++beats % 5U == 0U && inet_socket_timers_armed())) {
        work_queue(&net_service_work);
    }
    lock_release(&net_lock);
}

static void loopback_drain(void) {
    static int draining;
    if (draining) return;
    draining = 1;
    for (unsigned served = 0; served < LOOPBACK_BURST && loopback_first; served++) {
        struct loopback_packet *slot = loopback_first;
        loopback_first = slot->next;
        if (!loopback_first) loopback_last = NULL;
        loopback_bytes -= slot->length;
        stack_rx++;
        handle_ipv4(slot->data, slot->length);
        kfree(slot);
    }
    draining = 0;
}

static void run_tcp_timers(void) {
    static int timing;
    if (timing) return;
    timing = 1;
    inet_socket_tcp_timer_poll();
    timing = 0;
}

void net_poll(void) {
    NET_LOCKED;
    loopback_drain();
    if (adapter) adapter->poll(receive_frame);
    run_tcp_timers();
}
const struct net_config *net_get_config(void) { return &config; }
void net_set_address(uint32_t value) { config.address = value; }
void net_set_netmask(uint32_t value) { config.netmask = value; }
void net_set_gateway(uint32_t value) { config.gateway = value; }
void net_set_dns(uint32_t value) { config.dns = value; }
void net_set_interface_up(int up) { config.interface_up = up != 0; }
uint64_t net_rx_packets(void) { return stack_rx; }
uint64_t net_tx_packets(void) { return stack_tx; }
uint64_t net_rx_bytes(void) { return stack_rx_bytes; }
uint64_t net_tx_bytes(void) { return stack_tx_bytes; }

uint64_t net_rx_dropped(void) {
    NET_LOCKED;
    uint64_t adapter_drops = adapter && adapter->rx_dropped ? adapter->rx_dropped() : 0;
    return stack_drop + loopback_dropped + adapter_drops;
}

size_t net_arp_snapshot(struct net_arp_record *records, size_t capacity) {
    NET_LOCKED;
    size_t count = 0;
    for (unsigned i = 0; i < ARP_CACHE_SIZE; i++) {
        if (!arp_cache[i].ip) continue;
        if (records && count < capacity) {
            records[count].address = arp_cache[i].ip;
            memcpy(records[count].mac, arp_cache[i].mac, sizeof(records[count].mac));
        }
        count++;
    }
    return count;
}
