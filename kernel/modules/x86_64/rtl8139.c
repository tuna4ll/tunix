#include <stddef.h>
#include <stdint.h>
#include "../../include/apic.h"
#include "../../include/cpu.h"
#include "../../include/dma.h"
#include "../../include/heap.h"
#include "../../include/io.h"
#include "../../include/irq.h"
#include "../../include/kstring.h"
#include "../../include/module.h"
#include "../../include/pci.h"
#include "../../include/time.h"
#include "../../include/vmm.h"
#include "../../include/net/net.h"

#define RTL_VENDOR 0x10ECU
#define RTL_DEVICE 0x8139U
#define RX_RING_BYTES 32768U
#define RX_BUFFER_BYTES (RX_RING_BYTES + 16U + 1536U)
#define TX_BUFFER_BYTES 2048U

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

#define ISR_RX_OK 0x0001U
#define ISR_RX_ERROR 0x0002U
#define ISR_RX_OVERFLOW 0x0010U

#define RX_QUEUE_FRAMES 128U
#define RX_FRAME_BYTES 1536U

struct rx_frame {
    uint16_t length;
    uint8_t data[RX_FRAME_BYTES];
};

static struct rx_frame *rx_queue;
static volatile uint16_t rx_queue_head;
static volatile uint16_t rx_queue_tail;
static uint64_t queue_drop_count;
static unsigned rx_vector;

typedef void (*rtl8139_receive_fn)(const uint8_t *frame, size_t length);

static uint8_t *rx_buffer;
static uint64_t rx_physical;
static uint8_t *tx_buffer;
static uint64_t tx_physical;
static uint16_t io_base;
static uint8_t irq_pin;
static uint16_t rx_offset;
static unsigned tx_index;
static uint8_t mac_address[6];
static int available;
static uint64_t rx_count;
static uint64_t tx_count;
static uint64_t drop_count;

static const struct net_adapter rtl8139_adapter;

static int wait_clear(uint16_t port, uint8_t mask, uint64_t timeout_ns) {
    uint64_t deadline = time_uptime_ns() + timeout_ns;
    while (inb(port) & mask) {
        if (time_uptime_ns() >= deadline) return -1;
        cpu_relax();
    }
    return 0;
}

static int rtl8139_start(const struct pci_device *found) {
    struct pci_device device = *found;
    if (available) return -1;
    if (!(device.bar[0] & 1U)) return -1;
    io_base = (uint16_t)(device.bar[0] & ~3U);
    irq_pin = device.irq_line;
    pci_enable_bus_mastering(&device);

    if (!rx_buffer)
        rx_buffer = dma_alloc_below(RX_BUFFER_BYTES, 4096, DMA_LIMIT_32BIT, &rx_physical);
    if (!tx_buffer)
        tx_buffer = dma_alloc_below(4U * TX_BUFFER_BYTES, 256, DMA_LIMIT_32BIT,
                                    &tx_physical);
    if (!rx_buffer || !tx_buffer) return -1;

    outb((uint16_t)(io_base + REG_CONFIG1), 0x00U);
    outb((uint16_t)(io_base + REG_CMD), CMD_RESET);
    if (wait_clear((uint16_t)(io_base + REG_CMD), CMD_RESET, 100000000ULL) != 0) return -1;

    for (unsigned index = 0; index < 6; index++)
        mac_address[index] = inb((uint16_t)(io_base + REG_IDR0 + index));
    memset(rx_buffer, 0, RX_BUFFER_BYTES);
    memset(tx_buffer, 0, 4U * TX_BUFFER_BYTES);
    outl((uint16_t)(io_base + REG_RBSTART), (uint32_t)rx_physical);
    for (unsigned index = 0; index < 4; index++)
        outl((uint16_t)(io_base + REG_TSAD0 + index * 4U),
             (uint32_t)(tx_physical + index * TX_BUFFER_BYTES));
    outw((uint16_t)(io_base + REG_IMR), 0U);
    outw((uint16_t)(io_base + REG_ISR), 0xFFFFU);
    outl((uint16_t)(io_base + REG_RCR), RCR_CONFIG);
    outl((uint16_t)(io_base + REG_TCR), 0x03000700U);
    outb((uint16_t)(io_base + REG_CMD), CMD_RX_ENABLE | CMD_TX_ENABLE);
    rx_offset = 0;
    tx_index = 0;
    rx_count = tx_count = drop_count = 0;
    available = 1;
    return net_register_adapter(&rtl8139_adapter);
}

static void drain_card(void) {
    unsigned budget = 64;
    while (budget-- && !(inb((uint16_t)(io_base + REG_CMD)) & CMD_RX_EMPTY)) {
        uint8_t *entry = rx_buffer + rx_offset;
        uint16_t status = (uint16_t)(entry[0] | ((uint16_t)entry[1] << 8));
        uint16_t length = (uint16_t)(entry[2] | ((uint16_t)entry[3] << 8));
        if (!(status & 1U) || length < 18U || length > 1522U) {
            drop_count++;
            outb((uint16_t)(io_base + REG_CMD), CMD_RESET);
            if (wait_clear((uint16_t)(io_base + REG_CMD), CMD_RESET, 100000000ULL) == 0) {
                outl((uint16_t)(io_base + REG_RBSTART), (uint32_t)rx_physical);
                outl((uint16_t)(io_base + REG_RCR), RCR_CONFIG);
                outb((uint16_t)(io_base + REG_CMD), CMD_RX_ENABLE | CMD_TX_ENABLE);
            }
            rx_offset = 0;
            break;
        }

        size_t payload_length = (size_t)length - 4U;
        uint16_t next = (uint16_t)((rx_queue_tail + 1U) % RX_QUEUE_FRAMES);
        if (rx_queue && next != rx_queue_head && payload_length <= RX_FRAME_BYTES) {
            struct rx_frame *slot = &rx_queue[rx_queue_tail];
            memcpy(slot->data, entry + 4, payload_length);
            slot->length = (uint16_t)payload_length;
            __sync_synchronize();
            rx_queue_tail = next;
            rx_count++;
        } else {
            queue_drop_count++;
        }

        rx_offset = (uint16_t)((rx_offset + length + 4U + 3U) & ~3U);
        rx_offset %= RX_RING_BYTES;
        outw((uint16_t)(io_base + REG_CAPR), (uint16_t)(rx_offset - 16U));
    }
    outw((uint16_t)(io_base + REG_ISR), inw((uint16_t)(io_base + REG_ISR)));
}

static void rtl8139_interrupt(void *context) {
    (void)context;
    if (!available) return;
    uint16_t status = inw((uint16_t)(io_base + REG_ISR));
    outw((uint16_t)(io_base + REG_ISR), status);
    if (status & (ISR_RX_OK | ISR_RX_ERROR | ISR_RX_OVERFLOW)) drain_card();
}

static void rtl8139_enable_interrupt(void) {
    if (!available || rx_vector) return;
    if (!rx_queue) rx_queue = (struct rx_frame *)kmalloc(sizeof(*rx_queue) * RX_QUEUE_FRAMES);
    if (!rx_queue) return;
    rx_queue_head = rx_queue_tail = 0;

    unsigned vector = irq_request("rtl8139", "IO-APIC", rtl8139_interrupt, NULL);
    if (!vector) return;

    int routed = 0;
    if (irq_pin && irq_pin < 16U && apic_route_global(irq_pin, vector) == 0) routed++;
    for (unsigned input = 16U; input <= 19U; input++)
        if (apic_route_global(input, vector) == 0) routed++;
    if (!routed) return;

    rx_vector = vector;
    outw((uint16_t)(io_base + REG_IMR), ISR_RX_OK | ISR_RX_ERROR | ISR_RX_OVERFLOW);
}

static unsigned rtl8139_interrupt_vector(void) { return rx_vector; }
static uint64_t rtl8139_rx_dropped(void) { return drop_count + queue_drop_count; }

static int rtl8139_transmit(const void *frame, size_t length) {
    if (!available || !frame || length < 14U || length > 1514U) return -1;
    unsigned slot = tx_index++ & 3U;
    uint16_t status_port = (uint16_t)(io_base + REG_TSD0 + slot * 4U);
    uint64_t deadline = time_uptime_ns() + 100000000ULL;
    while (!(inl(status_port) & (1U << 13))) {
        if (time_uptime_ns() >= deadline) return -1;
        cpu_relax();
    }
    uint8_t *window = tx_buffer + slot * TX_BUFFER_BYTES;
    memcpy(window, frame, length);
    if (length < 60U) {
        memset(window + length, 0, 60U - length);
        length = 60U;
    }
    cpu_memory_barrier();
    outl(status_port, (uint32_t)length);
    tx_count++;
    return 0;
}

static void rtl8139_poll(rtl8139_receive_fn receive) {
    if (!available || !receive) return;
    if (!rx_vector) drain_card();

    unsigned served = 0;
    while (rx_queue && rx_queue_head != rx_queue_tail && served < 64U) {
        struct rx_frame *slot = &rx_queue[rx_queue_head];
        receive(slot->data, slot->length);
        rx_queue_head = (uint16_t)((rx_queue_head + 1U) % RX_QUEUE_FRAMES);
        served++;
    }
}

static const struct net_adapter rtl8139_adapter = {
    .name = "rtl8139",
    .mac = mac_address,
    .transmit = rtl8139_transmit,
    .poll = rtl8139_poll,
    .rx_dropped = rtl8139_rx_dropped,
    .enable_interrupts = rtl8139_enable_interrupt,
    .interrupt_vector = rtl8139_interrupt_vector,
};

static void rtl8139_stop(const struct pci_device *device) {
    (void)device;
    if (!available) return;
    outw((uint16_t)(io_base + REG_IMR), 0U);
    outb((uint16_t)(io_base + REG_CMD), 0U);
    available = 0;
    net_unregister_adapter(&rtl8139_adapter);
    if (rx_vector) {
        irq_release(rx_vector);
        rx_vector = 0;
    }
    kfree(rx_queue);
    rx_queue = NULL;
    dma_free(rx_buffer, RX_BUFFER_BYTES);
    dma_free(tx_buffer, 4U * TX_BUFFER_BYTES);
    rx_buffer = NULL;
    tx_buffer = NULL;
}

static const struct pci_device_id rtl8139_ids[] = {
    { RTL_VENDOR, RTL_DEVICE, PCI_ANY_ID, PCI_ANY_ID },
};

static struct pci_driver rtl8139_driver = {
    .name = "rtl8139",
    .ids = rtl8139_ids,
    .id_count = sizeof(rtl8139_ids) / sizeof(rtl8139_ids[0]),
    .probe = rtl8139_start,
    .remove = rtl8139_stop,
};

static int rtl8139_load(void) {
    return pci_register_driver(&rtl8139_driver);
}

static void rtl8139_unload(void) {
    pci_unregister_driver(&rtl8139_driver);
}

MODULE_MAIN(rtl8139_load, rtl8139_unload);
MODULE_PCI_ALIAS("10EC", "8139");
MODULE_LICENSE("MIT");
MODULE_DESCRIPTION("Realtek RTL8139 Ethernet controller");
MODULE_AUTHOR("Tunix");
