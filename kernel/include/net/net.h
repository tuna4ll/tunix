#ifndef TUNIX_NET_H
#define TUNIX_NET_H

#include <stddef.h>
#include <stdint.h>

#define NET_MTU 1500U

#define NET_LOOPBACK_NETWORK 0x7F000000U
#define NET_LOOPBACK_MASK    0xFF000000U
#define NET_LOOPBACK_ADDRESS 0x7F000001U

#define IPPROTO_TCP 6U
#define TCP_FIN 0x01U
#define TCP_SYN 0x02U
#define TCP_RST 0x04U
#define TCP_PSH 0x08U
#define TCP_ACK 0x10U

struct net_config {
    uint8_t mac[6];
    uint32_t address;
    uint32_t netmask;
    uint32_t gateway;
    uint32_t dns;
    int link_up;
    int interface_up;
};

struct net_arp_record {
    uint32_t address;
    uint8_t mac[6];
};

struct net_adapter {
    const char *name;
    const uint8_t *mac;
    int (*transmit)(const void *frame, size_t length);
    void (*poll)(void (*deliver)(const uint8_t *frame, size_t length));
    uint64_t (*rx_dropped)(void);
    void (*enable_interrupts)(void);
    unsigned (*interrupt_vector)(void);
};

int net_register_adapter(const struct net_adapter *card);
void net_unregister_adapter(const struct net_adapter *card);

void net_init(void);
void net_poll(void);
void net_enable_interrupts(void);
const struct net_config *net_get_config(void);
void net_set_address(uint32_t address);
void net_set_netmask(uint32_t netmask);
void net_set_gateway(uint32_t gateway);
void net_set_dns(uint32_t dns);
void net_set_interface_up(int up);
uint16_t net_checksum(const void *data, size_t length);
#define NET_IFINDEX_LO 1
#define NET_IFINDEX_ETH0 2

int net_interface_ioctl(unsigned long request, void *argument);

int net_is_loopback(uint32_t address);
uint32_t net_source_for(uint32_t destination);
uint16_t net_htons(uint16_t value);
uint32_t net_htonl(uint32_t value);
int net_send_ethernet(const uint8_t destination[6], uint16_t type, const void *payload, size_t length);
int net_send_raw_ethernet(const void *frame, size_t length);
int net_send_ipv4(uint32_t destination, uint8_t protocol, const void *payload, size_t length,
                  uint8_t ttl, int header_included);
int net_send_udp(uint32_t source, uint16_t source_port, uint32_t destination,
                 uint16_t destination_port, const void *payload, size_t length);
int net_send_tcp(uint32_t source, uint16_t source_port, uint32_t destination,
                 uint16_t destination_port, uint32_t seq, uint32_t ack, uint8_t flags,
                 uint16_t window, const void *payload, size_t length);
uint64_t net_rx_packets(void);
uint64_t net_tx_packets(void);
uint64_t net_rx_dropped(void);
size_t net_arp_snapshot(struct net_arp_record *records, size_t capacity);

#endif
