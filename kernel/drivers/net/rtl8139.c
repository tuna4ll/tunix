#include <stddef.h>
#include <stdint.h>
#include "../../include/apic.h"
#include "../../include/heap.h"
#include "../../include/io.h"
#include "../../include/irq.h"
#include "../../include/kstring.h"
#include "../../include/pci.h"
#include "../../include/time.h"
#include "../../include/vmm.h"
#include "../../include/net/rtl8139.h"

#define RTL_VENDOR 0x10ECU
#define RTL_DEVICE 0x8139U
/*
 * The hardware ring, which used to be drained only from net_poll() at syscall
 * time. Between two recv() calls a whole TCP window of back-to-back frames
 * could land here untouched; at 8 KiB it was smaller than a single 8 KiB TCP
 * window once framing is added, so a bulk download overran it, and the overrun
 * trips the invalid-descriptor path below, which resets the chip and throws the
 * *entire* ring away. 32 KiB gives several windows of slack. Keep RCR's RBLEN
 * field (below) in sync.
 *
 * The card interrupts now, and the handler empties this into the software
 * queue further down, so the slack is a margin rather than the whole defence.
 */
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

/* Interrupt status and mask share a layout. Only the receive half is asked
   for: a transmit is waited out in rtl8139_transmit() and finishes in the time
   it takes to ask. */
#define ISR_RX_OK 0x0001U
#define ISR_RX_ERROR 0x0002U
#define ISR_RX_OVERFLOW 0x0010U

/*
 * Where a frame goes between the card and the network stack.
 *
 * The interrupt cannot hand a frame straight to the stack. It arrives inside
 * whatever the processor was doing, which may be a system call already halfway
 * through that same stack -- the kernel lock does not separate them, because
 * the interrupt runs *inside* the lock its victim is holding. So the handler
 * does the part that has a deadline (getting frames out of the card before the
 * ring wraps over them) and leaves the part that does not to net_poll().
 *
 * A frame is dropped when the queue is full, which is the honest failure: the
 * stack is not keeping up, and one frame lost from the tail is a great deal
 * better than the reset that losing the hardware ring costs.
 */
#define RX_QUEUE_FRAMES 128U
#define RX_FRAME_BYTES 1536U

struct rx_frame {
    uint16_t length;
    uint8_t data[RX_FRAME_BYTES];
};

static struct rx_frame *rx_queue;
/* Written by the handler, read by the drain, and never both at once for the
   same slot. Neither index is guarded: they are 16-bit, each has exactly one
   writer, and the reader of the other side's index only has to see a value
   that was true at some point. */
static volatile uint16_t rx_queue_head;
static volatile uint16_t rx_queue_tail;
static uint64_t queue_drop_count;
static unsigned rx_vector;

static uint8_t rx_buffer[RX_BUFFER_BYTES] __attribute__((aligned(4096)));
static uint8_t tx_buffer[4][TX_BUFFER_BYTES] __attribute__((aligned(256)));
static uint16_t io_base;
static uint8_t irq_pin;
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
    irq_pin = device.irq_line;
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

/*
 * Take everything the card has and put it in the queue.
 *
 * Shared by the interrupt and by net_poll(), because a machine whose interrupt
 * never arrives has to keep working: the poll is now a safety net rather than
 * the only path. The budget bounds one visit, not the ring.
 */
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
                outl((uint16_t)(io_base + REG_RBSTART),
                     (uint32_t)vmm_virt_to_phys_direct(rx_buffer));
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
            /* The frame before the index that publishes it. */
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

/*
 * Acknowledged before anything else is done with it, and unconditionally.
 *
 * The line is level-triggered and shared, the way every PCI interrupt pin is:
 * it stays asserted until the card's own status register is cleared, so a
 * handler that returns without clearing it is asked again immediately, for
 * ever. That is not a slow machine, it is a stopped one.
 */
static void rtl8139_interrupt(void *context) {
    (void)context;
    if (!available) return;
    uint16_t status = inw((uint16_t)(io_base + REG_ISR));
    outw((uint16_t)(io_base + REG_ISR), status);
    if (status & (ISR_RX_OK | ISR_RX_ERROR | ISR_RX_OVERFLOW)) drain_card();
}

/*
 * Ask the card to say when a frame arrives.
 *
 * Failing here is not fatal and is not even reported: without it the driver is
 * what it was, a card drained at syscall time. What it costs then is what it
 * always cost, which is why this is worth having and not worth panicking over.
 */
void rtl8139_enable_interrupt(void) {
    if (!available || rx_vector) return;
    if (!rx_queue) rx_queue = (struct rx_frame *)kmalloc(sizeof(*rx_queue) * RX_QUEUE_FRAMES);
    if (!rx_queue) return;
    rx_queue_head = rx_queue_tail = 0;

    unsigned vector = irq_request("rtl8139", "IO-APIC", rtl8139_interrupt, NULL);
    if (!vector) return;

    /*
     * Every input the card could be on, all pointed at the one handler.
     *
     * The card has no MSI capability, so the only way in is the interrupt pin,
     * and which IOAPIC input that pin reaches is described in ACPI's _PRT --
     * which is AML, which this kernel does not interpret. The number in config
     * space is the answer the *8259* would have wanted, and on q35 it is not
     * the IOAPIC's: PCI Express slots land on inputs 16 through 19 there, so
     * routing the config-space number alone routes nothing, silently.
     *
     * Listening to all of them is honest here. A pin is level-triggered and
     * shared by design, so a handler has to identify its own device anyway --
     * this one reads the card's status register and returns when the card has
     * nothing to say. And nothing else in this kernel unmasks a device
     * interrupt, so no other line can be asserted for this to sit under.
     */
    int routed = 0;
    if (irq_pin && irq_pin < 16U && apic_route_global(irq_pin, vector) == 0) routed++;
    for (unsigned input = 16U; input <= 19U; input++)
        if (apic_route_global(input, vector) == 0) routed++;
    if (!routed) return;

    rx_vector = vector;
    outw((uint16_t)(io_base + REG_IMR), ISR_RX_OK | ISR_RX_ERROR | ISR_RX_OVERFLOW);
}

unsigned rtl8139_interrupt_vector(void) { return rx_vector; }
uint64_t rtl8139_queue_dropped(void) { return queue_drop_count; }

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

/*
 * Hand queued frames to the stack, and sweep the card as well.
 *
 * The sweep stays because the queue is only as good as the interrupt behind
 * it: on a machine where the line never arrives -- a firmware that describes
 * the pin differently, an IOAPIC this kernel did not understand -- this is
 * still the whole receive path, exactly as it was.
 *
 * Only this side moves rx_queue_head, and it is the only caller allowed into
 * the network stack, which is what keeps a frame arriving mid-call out of it.
 */
void rtl8139_poll(rtl8139_receive_fn receive) {
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
