#ifndef TUNIX_RTL8139_H
#define TUNIX_RTL8139_H

#include <stddef.h>
#include <stdint.h>

typedef void (*rtl8139_receive_fn)(const uint8_t *frame, size_t length);

int rtl8139_init(void);
int rtl8139_present(void);
/* Give the card an interrupt, after net_init() has one. Quietly does nothing
   where there is no vector to be had; see rtl8139.c. */
void rtl8139_enable_interrupt(void);
unsigned rtl8139_interrupt_vector(void);
/* Frames the card delivered that the queue had no room for, which is the
   stack falling behind rather than the wire. */
uint64_t rtl8139_queue_dropped(void);
const uint8_t *rtl8139_mac(void);
int rtl8139_transmit(const void *frame, size_t length);
void rtl8139_poll(rtl8139_receive_fn receive);
uint64_t rtl8139_rx_packets(void);
uint64_t rtl8139_tx_packets(void);
uint64_t rtl8139_rx_dropped(void);

#endif
