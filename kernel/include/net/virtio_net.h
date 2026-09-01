#ifndef TUNIX_VIRTIO_NET_H
#define TUNIX_VIRTIO_NET_H

#include <stddef.h>
#include <stdint.h>

typedef void (*virtio_net_receive_fn)(const uint8_t *frame, size_t length);

int virtio_net_init(void);
int virtio_net_present(void);
const uint8_t *virtio_net_mac(void);
int virtio_net_transmit(const void *frame, size_t length);
void virtio_net_poll(virtio_net_receive_fn receive);
unsigned virtio_net_interrupt_vector(void);
uint64_t virtio_net_rx_packets(void);
uint64_t virtio_net_tx_packets(void);
uint64_t virtio_net_rx_dropped(void);

#endif
