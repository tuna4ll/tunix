#ifndef TUNIX_RTL8139_H
#define TUNIX_RTL8139_H

#include <stddef.h>
#include <stdint.h>

typedef void (*rtl8139_receive_fn)(const uint8_t *frame, size_t length);

int rtl8139_init(void);
int rtl8139_present(void);
const uint8_t *rtl8139_mac(void);
int rtl8139_transmit(const void *frame, size_t length);
void rtl8139_poll(rtl8139_receive_fn receive);
uint64_t rtl8139_rx_packets(void);
uint64_t rtl8139_tx_packets(void);
uint64_t rtl8139_rx_dropped(void);

#endif
