#include <stddef.h>
#include <stdint.h>
#include "../include/io.h"
#include "../include/kstring.h"
#include "../include/pci.h"
#include "../include/time.h"
#include "../include/vmm.h"
#include "../include/net/rtl8139.h"

#define RTL_VENDOR 0x10ECU
#define RTL_DEVICE 0x8139U
/* The RX ring is drained only from net_poll() at syscall time -- there is no
 * RX interrupt -- so between two recv() calls a whole TCP window's worth of
 * back-to-back frames can land here untouched. At 8 KiB the ring was smaller
 * than one 8 KiB TCP window once Ethernet/IP/TCP framing is added, so a bulk
 * download (git clone) overran it; the overrun then trips the invalid-descriptor
 * path in rtl8139_poll(), which resets the chip and discards the *entire* ring,
 * losing a chunk of the stream and eventually killing the connection. 32 KiB
 * gives several windows of slack. Keep RCR's RBLEN field (below) in sync. */
#define RX_RING_BYTES 32768U
#define RX_BUFFER_BYTES (RX_RING_BYTES + 16U + 1536U)
#define TX_BUFFER_BYTES 2048U

/* Receive Configuration Register: accept broadcast/multicast/physical-match and
 * run promiscuous (AAP), no RX threshold, max DMA burst, WRAP=1, and RBLEN in
 * bits [12:11] selecting the ring size. RBLEN encodes 8K=0b00, 16K=0b01,
 * 32K=0b10, 64K=0b11; 0x1000 is the 32K setting matching RX_RING_BYTES above.
 * (Was 0xE78F = 8K.) */
#define RCR_CONFIG 0x0000F78FU

#define REG_IDR0 0x00U
#define REG_TSD0 0x10U
#define REG_TSAD0 0x20U
#define REG_RBSTART 0x30U
#define REG_CMD 0x37U
#define REG_CAPR 0x38U
#define REG_IMR 0x3CU
#define REG_ISR 0x3EU
#define REG_TCR 0x40U
#define REG_RCR 0x44U
#define REG_CONFIG1 0x52U

#define CMD_RESET 0x10U
#define CMD_RX_ENABLE 0x08U
#define CMD_TX_ENABLE 0x04U
#define CMD_RX_EMPTY 0x01U

static uint8_t rx_buffer[RX_BUFFER_BYTES] __attribute__((aligned(4096)));
static uint8_t tx_buffer[4][TX_BUFFER_BYTES] __attribute__((aligned(256)));
static uint16_t io_base;
static uint16_t rx_offset;
static unsigned tx_index;
static uint8_t mac_address[6];
static int available;
static uint64_t rx_count;
static uint64_t tx_count;
static uint64_t drop_count;

static int wait_clear(uint16_t port, uint8_t mask, uint64_t timeout_ns) {
    uint64_t deadline = time_uptime_ns() + timeout_ns;
    while (inb(port) & mask) {
        if (time_uptime_ns() >= deadline) return -1;
        __asm__ volatile("pause");
    }
    return 0;
}

int rtl8139_init(void) {
    struct pci_device device;
    available = 0;
    if (pci_find_device(RTL_VENDOR, RTL_DEVICE, &device) != 0) return -1;
    if (!(device.bar[0] & 1U)) return -1;
    io_base = (uint16_t)(device.bar[0] & ~3U);
    pci_enable_bus_mastering(&device);

    outb((uint16_t)(io_base + REG_CONFIG1), 0x00U);
    outb((uint16_t)(io_base + REG_CMD), CMD_RESET);
    if (wait_clear((uint16_t)(io_base + REG_CMD), CMD_RESET, 100000000ULL) != 0) return -1;

    for (unsigned index = 0; index < 6; index++)
        mac_address[index] = inb((uint16_t)(io_base + REG_IDR0 + index));
    memset(rx_buffer, 0, sizeof(rx_buffer));
    memset(tx_buffer, 0, sizeof(tx_buffer));
    outl((uint16_t)(io_base + REG_RBSTART), (uint32_t)vmm_virt_to_phys_direct(rx_buffer));
    for (unsigned index = 0; index < 4; index++)
        outl((uint16_t)(io_base + REG_TSAD0 + index * 4U),
             (uint32_t)vmm_virt_to_phys_direct(tx_buffer[index]));
    outw((uint16_t)(io_base + REG_IMR), 0U);
    outw((uint16_t)(io_base + REG_ISR), 0xFFFFU);
    outl((uint16_t)(io_base + REG_RCR), RCR_CONFIG);
    outl((uint16_t)(io_base + REG_TCR), 0x03000700U);
    outb((uint16_t)(io_base + REG_CMD), CMD_RX_ENABLE | CMD_TX_ENABLE);
    rx_offset = 0;
    tx_index = 0;
    rx_count = tx_count = drop_count = 0;
    available = 1;
    return 0;
}

int rtl8139_present(void) { return available; }
const uint8_t *rtl8139_mac(void) { return mac_address; }
uint64_t rtl8139_rx_packets(void) { return rx_count; }
uint64_t rtl8139_tx_packets(void) { return tx_count; }
uint64_t rtl8139_rx_dropped(void) { return drop_count; }

int rtl8139_transmit(const void *frame, size_t length) {
    if (!available || !frame || length < 14U || length > 1514U) return -1;
    unsigned slot = tx_index++ & 3U;
    uint16_t status_port = (uint16_t)(io_base + REG_TSD0 + slot * 4U);
    uint64_t deadline = time_uptime_ns() + 100000000ULL;
    while (!(inl(status_port) & (1U << 13))) {
        if (time_uptime_ns() >= deadline) return -1;
        __asm__ volatile("pause");
    }
    memcpy(tx_buffer[slot], frame, length);
    if (length < 60U) {
        memset(tx_buffer[slot] + length, 0, 60U - length);
        length = 60U;
    }
    __asm__ volatile("mfence" : : : "memory");
    outl(status_port, (uint32_t)length);
    tx_count++;
    return 0;
}

void rtl8139_poll(rtl8139_receive_fn receive) {
    if (!available || !receive) return;
    unsigned budget = 64;
    while (budget-- && !(inb((uint16_t)(io_base + REG_CMD)) & CMD_RX_EMPTY)) {
        uint8_t *entry = rx_buffer + rx_offset;
        uint16_t status = (uint16_t)(entry[0] | ((uint16_t)entry[1] << 8));
        uint16_t length = (uint16_t)(entry[2] | ((uint16_t)entry[3] << 8));
        if (!(status & 1U) || length < 18U || length > 1522U) {
            drop_count++;
            outb((uint16_t)(io_base + REG_CMD), CMD_RESET);
            if (wait_clear((uint16_t)(io_base + REG_CMD), CMD_RESET, 100000000ULL) == 0) {
                outl((uint16_t)(io_base + REG_RBSTART), (uint32_t)vmm_virt_to_phys_direct(rx_buffer));
                outl((uint16_t)(io_base + REG_RCR), RCR_CONFIG);
                outb((uint16_t)(io_base + REG_CMD), CMD_RX_ENABLE | CMD_TX_ENABLE);
            }
            rx_offset = 0;
            break;
        }
        size_t payload_length = (size_t)length - 4U;
        receive(entry + 4, payload_length);
        rx_count++;
        rx_offset = (uint16_t)((rx_offset + length + 4U + 3U) & ~3U);
        rx_offset %= RX_RING_BYTES;
        outw((uint16_t)(io_base + REG_CAPR), (uint16_t)(rx_offset - 16U));
    }
    outw((uint16_t)(io_base + REG_ISR), inw((uint16_t)(io_base + REG_ISR)));
}
