#include <stddef.h>
#include <stdint.h>
#include "../include/kstring.h"
#include "../include/time.h"
#include "../include/net/inet_socket.h"
#include "../include/net/net.h"
#include "../include/net/rtl8139.h"

extern void kprintf(const char *fmt, ...);

#define ETHERTYPE_IPV4 0x0800U
#define ETHERTYPE_ARP 0x0806U
#define IPPROTO_ICMP 1U
#define IPPROTO_UDP 17U
#define ARP_CACHE_SIZE 16

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

static struct net_config config;
static struct arp_entry arp_cache[ARP_CACHE_SIZE];
static unsigned arp_replace;
static uint16_t ipv4_identification;
static uint64_t stack_rx;
static uint64_t stack_tx;
static uint64_t stack_drop;

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
    if (!config.interface_up || length > NET_MTU) return -1;
    uint8_t frame[1514];
    struct ethernet_header *header = (struct ethernet_header *)frame;
    memcpy(header->destination, destination, 6);
    memcpy(header->source, config.mac, 6);
    header->type = net_htons(type);
    memcpy(frame + sizeof(*header), payload, length);
    if (rtl8139_transmit(frame, sizeof(*header) + length) != 0) return -1;
    stack_tx++;
    return 0;
}

int net_send_raw_ethernet(const void *frame, size_t length) {
    if (!config.interface_up || !frame || length < 14U || length > 1514U) return -1;
    if (rtl8139_transmit(frame, length) != 0) return -1;
    stack_tx++;
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
        __asm__ volatile("pause");
    }
    return NULL;
}

int net_send_ipv4(uint32_t destination, uint8_t protocol, const void *payload, size_t length,
                  uint8_t ttl, int header_included) {
    if (!payload || !config.interface_up) return -1;
    if (header_included) {
        if (length < sizeof(struct ipv4_header) || length > NET_MTU) return -1;
        const struct ipv4_header *provided = (const struct ipv4_header *)payload;
        const uint8_t *mac = resolve_mac(provided->destination);
        return mac ? net_send_ethernet(mac, ETHERTYPE_IPV4, payload, length) : -1;
    }
    if (length + sizeof(struct ipv4_header) > NET_MTU) return -1;
    uint8_t packet[NET_MTU];
    struct ipv4_header *header = (struct ipv4_header *)packet;
    memset(header, 0, sizeof(*header));
    header->version_ihl = 0x45U;
    header->total_length = net_htons((uint16_t)(sizeof(*header) + length));
    header->identification = net_htons(++ipv4_identification);
    header->fragment = net_htons(0x4000U);
    header->ttl = ttl ? ttl : 64U;
    header->protocol = protocol;
    header->source = config.address;
    header->destination = destination;
    header->checksum = net_htons(net_checksum(header, sizeof(*header)));
    memcpy(packet + sizeof(*header), payload, length);
    const uint8_t *mac = resolve_mac(destination);
    return mac ? net_send_ethernet(mac, ETHERTYPE_IPV4, packet, sizeof(*header) + length) : -1;
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
    if (length + sizeof(struct udp_header) > NET_MTU - sizeof(struct ipv4_header)) return -1;
    uint8_t packet[NET_MTU];
    struct udp_header *header = (struct udp_header *)packet;
    header->source_port = net_htons(source_port);
    header->destination_port = net_htons(destination_port);
    header->length = net_htons((uint16_t)(sizeof(*header) + length));
    header->checksum = 0;
    memcpy(packet + sizeof(*header), payload, length);
    uint32_t actual_source = source ? source : config.address;
    header->checksum = net_htons(udp_checksum(actual_source, destination, packet,
                                              sizeof(*header) + length));
    return net_send_ipv4(destination, IPPROTO_UDP, packet, sizeof(*header) + length, 64, 0);
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
    if (icmp->type == 8 && icmp->code == 0 && ip->destination == config.address) {
        uint8_t reply[NET_MTU];
        memcpy(reply, data, length);
        struct icmp_header *response = (struct icmp_header *)reply;
        response->type = 0;
        response->checksum = 0;
        response->checksum = net_htons(net_checksum(reply, length));
        (void)net_send_ipv4(ip->source, IPPROTO_ICMP, reply, length, 64, 0);
    }
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
    if (net_htons(ip->fragment) & 0x3FFFU) { stack_drop++; return; }
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
    inet_socket_receive_ethernet(frame, length, type);
    const uint8_t *payload = frame + sizeof(*header);
    size_t payload_length = length - sizeof(*header);
    if (type == ETHERTYPE_ARP) handle_arp(payload, payload_length);
    else if (type == ETHERTYPE_IPV4) handle_ipv4(payload, payload_length);
}

void net_init(void) {
    memset(&config, 0, sizeof(config));
    memset(arp_cache, 0, sizeof(arp_cache));
    /* QEMU user networking defaults. udhcpc/ifconfig can replace these. */
    config.address = net_htonl(0x0A00020FU);
    config.netmask = net_htonl(0xFFFFFF00U);
    config.gateway = net_htonl(0x0A000202U);
    config.dns = net_htonl(0x0A000203U);
    if (rtl8139_init() == 0) {
        memcpy(config.mac, rtl8139_mac(), 6);
        config.link_up = 1;
        config.interface_up = 1;
        kprintf("NET: rtl8139 eth0 %x:%x:%x:%x:%x:%x ready\n",
                config.mac[0], config.mac[1], config.mac[2], config.mac[3], config.mac[4], config.mac[5]);
    } else {
        kprintf("NET: no RTL8139 adapter found\n");
    }
}

void net_poll(void) { if (config.link_up) rtl8139_poll(receive_frame); }
const struct net_config *net_get_config(void) { return &config; }
void net_set_address(uint32_t value) { config.address = value; }
void net_set_netmask(uint32_t value) { config.netmask = value; }
void net_set_gateway(uint32_t value) { config.gateway = value; }
void net_set_dns(uint32_t value) { config.dns = value; }
void net_set_interface_up(int up) { config.interface_up = up != 0; }
uint64_t net_rx_packets(void) { return stack_rx; }
uint64_t net_tx_packets(void) { return stack_tx; }
uint64_t net_rx_dropped(void) { return stack_drop + rtl8139_rx_dropped(); }

size_t net_arp_snapshot(struct net_arp_record *records, size_t capacity) {
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
