/*
 * xHCI: the USB host controller every machine built this decade has, and the
 * only way to reach a keyboard on one that has no PS/2 port left.
 *
 * This is the bring-up half -- find the controller, map its registers, reset
 * it and start it. What runs on top of it (rings, devices, HID) is layered
 * above and depends on nothing here beyond the register accessors.
 *
 * Two things about xHCI shape the code. Its registers live in four separate
 * blocks whose offsets are read out of the first one, so nothing can be a
 * fixed address; and the controller answers "not ready" for a while after a
 * reset, which is a state to wait out rather than an error.
 */
#include <stdint.h>
#include <stddef.h>

#include "../include/pci.h"
#include "../include/pmm.h"
#include "../include/vmm.h"
#include "../include/time.h"
#include "../include/kstring.h"
#include "../include/input.h"
#include "../../include/tunix/input_event.h"
#include "../include/xhci.h"

extern void kprintf(const char *fmt, ...);

/* PCI class 0x0C.03.30 is "serial bus / USB / xHCI". The programming
   interface matters: the same class and subclass also cover UHCI, OHCI and
   EHCI, which share nothing with this. */
#define PCI_CLASS_SERIAL_BUS 0x0CU
#define PCI_SUBCLASS_USB 0x03U
#define PCI_PROG_IF_XHCI 0x30U

#define PCI_BAR_TYPE_MASK 0x6U
#define PCI_BAR_TYPE_64BIT 0x4U
#define PCI_BAR_IO 0x1U
#define PCI_BAR_ADDRESS_MASK 0xFFFFFFF0U
#define PCI_BAR0_OFFSET 0x10U

/* Capability registers, at the start of the mapping. */
/* CAPLENGTH is a byte and HCIVERSION the halfword above it, so both come out
   of one aligned dword: a 32-bit read at 0x02 would straddle into HCSPARAMS1
   and hand back a plausible-looking version that is really a slot count. */
#define XHCI_CAPLENGTH 0x00U
#define XHCI_VERSION_SHIFT 16U
#define XHCI_HCSPARAMS1 0x04U
#define XHCI_HCSPARAMS2 0x08U
#define XHCI_HCCPARAMS1 0x10U
#define XHCI_DBOFF 0x14U
#define XHCI_RTSOFF 0x18U

#define HCSPARAMS1_MAX_SLOTS_SHIFT 0U
#define HCSPARAMS1_MAX_SLOTS_MASK 0xFFU
#define HCSPARAMS1_MAX_PORTS_SHIFT 24U
#define HCSPARAMS1_MAX_PORTS_MASK 0xFFU
#define HCCPARAMS1_CONTEXT_64 (1U << 2)

#define HCSPARAMS2_SCRATCHPAD_HIGH_SHIFT 21U
#define HCSPARAMS2_SCRATCHPAD_HIGH_MASK 0x1FU
#define HCSPARAMS2_SCRATCHPAD_LOW_SHIFT 27U
#define HCSPARAMS2_SCRATCHPAD_LOW_MASK 0x1FU

/* Operational registers, at CAPLENGTH from the base. */
#define XHCI_USBCMD 0x00U
#define XHCI_USBSTS 0x04U
#define XHCI_PAGESIZE 0x08U
#define XHCI_CRCR 0x18U
#define XHCI_DCBAAP 0x30U
#define XHCI_CONFIG 0x38U

/* Interrupter 0, the only one this driver uses, at RTSOFF + 0x20. */
#define XHCI_INTERRUPTER0 0x20U
#define XHCI_IMAN 0x00U
#define XHCI_IMOD 0x04U
#define XHCI_ERSTSZ 0x08U
#define XHCI_ERSTBA 0x10U
#define XHCI_ERDP 0x18U

#define CRCR_RING_CYCLE_STATE (1ULL << 0)
/* Written back with the dequeue pointer to say the handler is done. */
#define ERDP_EVENT_HANDLER_BUSY (1ULL << 3)

/* A TRB is four dwords. The type lives in the top of the last one, and the
   bottom bit of that dword is the cycle bit -- the only thing that
   distinguishes an entry the controller has written from one left over from
   the ring's previous lap. */
#define TRB_BYTES 16U
#define TRB_TYPE_SHIFT 10U
#define TRB_TYPE_MASK 0x3FU
#define TRB_CYCLE (1U << 0)
#define TRB_TOGGLE_CYCLE (1U << 1)

#define TRB_TYPE_NORMAL 1U
#define TRB_TYPE_SETUP_STAGE 2U
#define TRB_TYPE_DATA_STAGE 3U
#define TRB_TYPE_STATUS_STAGE 4U
#define TRB_TYPE_LINK 6U
#define TRB_TYPE_ENABLE_SLOT 9U
#define TRB_TYPE_ADDRESS_DEVICE 11U
#define TRB_TYPE_NO_OP_COMMAND 23U
#define TRB_TYPE_TRANSFER_EVENT 32U
#define TRB_TYPE_COMMAND_COMPLETION 33U
#define TRB_TYPE_PORT_STATUS_CHANGE 34U

#define TRB_COMPLETION_SHIFT 24U
#define TRB_COMPLETION_MASK 0xFFU
#define TRB_COMPLETION_SUCCESS 1U

/* One page each: 256 TRBs is far more than this driver ever has outstanding,
   and a page is the smallest thing the physical allocator hands out anyway. */
#define RING_BYTES 4096U
#define RING_TRB_COUNT (RING_BYTES / TRB_BYTES)
#define EVENT_RING_SEGMENTS 1U
#define ERST_ENTRY_BYTES 16U

#define COMMAND_TIMEOUT_NS (1000ULL * 1000ULL * 1000ULL)
#define RUN_TIMEOUT_NS (1000ULL * 1000ULL * 1000ULL)
#define PORT_RESET_TIMEOUT_NS (500ULL * 1000ULL * 1000ULL)
/* The specification's recovery time after a port reset, before the device is
   required to answer anything. */
#define PORT_SETTLE_NS (20ULL * 1000ULL * 1000ULL)

#define XHCI_PORTSC_BASE 0x400U
#define XHCI_PORT_STRIDE 0x10U
#define PORTSC_CONNECTED (1U << 0)
#define PORTSC_ENABLED (1U << 1)
#define PORTSC_RESET (1U << 4)
#define PORTSC_POWER (1U << 9)
#define PORTSC_SPEED_SHIFT 10U
#define PORTSC_SPEED_MASK 0xFU
/* Every status-change bit, and the enable bit, are write-one-to-clear. A plain
   read-modify-write of this register therefore disables the port and throws
   away the changes it was about to report; both have to be masked out first. */
#define PORTSC_CHANGE_MASK 0x00FE0000U
#define PORTSC_PRESERVE_MASK (~(PORTSC_CHANGE_MASK | PORTSC_ENABLED))

#define USB_SPEED_FULL 1U
#define USB_SPEED_LOW 2U
#define USB_SPEED_HIGH 3U
#define USB_SPEED_SUPER 4U

/* What a control endpoint may carry before the device has been asked. Low and
   full speed must start at eight; the real figure comes from the descriptor. */
#define EP0_PACKET_LOW_FULL 8U
#define EP0_PACKET_HIGH 64U
#define EP0_PACKET_SUPER 512U

/* Input context: a control context, then the slot, then the endpoints. */
#define INPUT_CONTROL_INDEX 0U
#define SLOT_CONTEXT_INDEX 1U
#define EP0_CONTEXT_INDEX 2U
#define INPUT_ADD_SLOT (1U << 0)
#define INPUT_ADD_EP0 (1U << 1)

#define SLOT_SPEED_SHIFT 20U
#define SLOT_CONTEXT_ENTRIES_SHIFT 27U
#define SLOT_ROOT_PORT_SHIFT 16U

#define EP_TYPE_CONTROL 4U
#define EP_TYPE_SHIFT 3U
#define EP_ERROR_COUNT_SHIFT 1U
#define EP_ERROR_COUNT 3U
#define EP_MAX_PACKET_SHIFT 16U
#define EP_DEQUEUE_CYCLE 1U

#define COMMAND_SLOT_SHIFT 24U

#define MAX_DEVICES 8U

/* Control transfer stages. The setup packet travels inside the TRB rather
   than through a buffer, which is what the immediate-data bit says. */
#define TRB_IMMEDIATE_DATA (1U << 6)
#define TRB_INTERRUPT_ON_COMPLETION (1U << 5)
#define TRB_CHAIN (1U << 4)
#define TRB_DIRECTION_IN (1U << 16)
#define TRB_TRANSFER_TYPE_SHIFT 16U
#define TRB_TRANSFER_TYPE_NO_DATA 0U
#define TRB_TRANSFER_TYPE_OUT 2U
#define TRB_TRANSFER_TYPE_IN 3U
#define SETUP_PACKET_BYTES 8U

/* Endpoint 0 is device context index 1: the index counts endpoints in the
   order the contexts appear, and the slot context occupies index 0. */
#define EP0_DOORBELL_TARGET 1U
#define DOORBELL_STRIDE 4U

#define USB_REQUEST_GET_DESCRIPTOR 6U
#define USB_REQUEST_SET_CONFIGURATION 9U
#define USB_DIRECTION_IN 0x80U
#define USB_DESCRIPTOR_DEVICE 1U
#define USB_DESCRIPTOR_TYPE_SHIFT 8U
#define USB_DEVICE_DESCRIPTOR_BYTES 18U
/* Offsets into the device descriptor that this driver reads. */
#define DEVICE_DESCRIPTOR_MAX_PACKET 7U

#define TRANSFER_TIMEOUT_NS (1000ULL * 1000ULL * 1000ULL)

#define USB_DESCRIPTOR_CONFIGURATION 2U
#define USB_DESCRIPTOR_INTERFACE 4U
#define USB_DESCRIPTOR_ENDPOINT 5U
#define CONFIGURATION_DESCRIPTOR_BYTES 9U
#define CONFIGURATION_TOTAL_LENGTH_OFFSET 2U
#define CONFIGURATION_VALUE_OFFSET 5U
#define DESCRIPTOR_LENGTH_OFFSET 0U
#define DESCRIPTOR_TYPE_OFFSET 1U
#define INTERFACE_NUMBER_OFFSET 2U
#define INTERFACE_CLASS_OFFSET 5U
#define INTERFACE_SUBCLASS_OFFSET 6U
#define INTERFACE_PROTOCOL_OFFSET 7U
#define ENDPOINT_ADDRESS_OFFSET 2U
#define ENDPOINT_ATTRIBUTES_OFFSET 3U
#define ENDPOINT_MAX_PACKET_OFFSET 4U
#define ENDPOINT_INTERVAL_OFFSET 6U

/* The HID boot subclass is the reason this driver can read a keyboard without
   understanding report descriptors at all: a device that advertises it must
   also answer in a fixed eight-byte format that predates the whole HID
   report machinery, and every keyboard and mouse supports it. */
#define USB_CLASS_HID 3U
#define HID_SUBCLASS_BOOT 1U
#define HID_PROTOCOL_KEYBOARD 1U
#define HID_PROTOCOL_MOUSE 2U

#define ENDPOINT_DIRECTION_IN 0x80U
#define ENDPOINT_NUMBER_MASK 0x0FU
#define ENDPOINT_TYPE_MASK 0x03U
#define ENDPOINT_TYPE_INTERRUPT 3U
#define ENDPOINT_TYPE_BULK 2U

/* Mass storage, bulk-only transport, SCSI command set: the combination every
   USB stick reports and the only one this driver answers. */
#define USB_CLASS_MASS_STORAGE 8U
#define MSC_SUBCLASS_SCSI 6U
#define MSC_PROTOCOL_BULK_ONLY 0x50U
#define EP_TYPE_BULK_OUT 2U
#define EP_TYPE_BULK_IN 6U

#define TRB_TYPE_CONFIGURE_ENDPOINT 12U
#define EP_TYPE_INTERRUPT_IN 7U
#define EP_INTERVAL_SHIFT 16U
#define EP_MAX_INTERVAL 15U
#define EP_AVERAGE_TRB_SHIFT 0U
#define EP_MAX_ESIT_SHIFT 16U
/* Full and low speed state the interval in frames; the field here counts in
   125-microsecond microframes, of which a frame holds eight. */
#define MICROFRAMES_PER_FRAME 8U

/* HID class requests. SET_PROTOCOL(0) is what puts a device into the fixed
   report format this driver can read without a report-descriptor parser. */
#define HID_REQUEST_TYPE_OUT 0x21U
#define HID_REQUEST_SET_PROTOCOL 0x0BU
#define HID_PROTOCOL_BOOT 0U

#define KEYBOARD_REPORT_BYTES 8U
#define KEYBOARD_REPORT_KEYS 6U
#define KEYBOARD_REPORT_KEYS_OFFSET 2U
#define KEYBOARD_MODIFIER_COUNT 8U
/* Usages below this are error conditions the device reports in every slot at
   once (rollover, POST fail); they are not keys. */
#define HID_USAGE_FIRST_KEY 4U

#define MOUSE_BUTTON_MASK 0x07U

#define USBCMD_RUN (1U << 0)
#define USBCMD_RESET (1U << 1)
#define USBSTS_HALTED (1U << 0)
#define USBSTS_HOST_ERROR (1U << 2)
#define USBSTS_NOT_READY (1U << 11)

/* The specification's own limits: 20 ms for a reset to complete, and the
   controller may hold "not ready" for a while after that. */
#define XHCI_RESET_TIMEOUT_NS (1000ULL * 1000ULL * 1000ULL)
#define XHCI_HALT_TIMEOUT_NS (100ULL * 1000ULL * 1000ULL)

#define MMIO_PAGE_BYTES 4096ULL
/* Enough for the capability, operational, runtime and doorbell blocks of any
   controller: the doorbell array is one dword per slot, the runtime block one
   interrupter set per interrupter, and both are bounded by the slot count. */
#define XHCI_REGISTER_BYTES 0x10000ULL

struct trb {
    uint32_t parameter_low;
    uint32_t parameter_high;
    uint32_t status;
    uint32_t control;
};

/* A producer ring the driver writes and the controller reads. The cycle bit
   is what says "this entry is mine now"; it flips every lap, so the ring never
   needs clearing and the controller never mistakes a stale entry for a new
   one. The last slot is always a Link back to the start. */
struct producer_ring {
    struct trb *entries;
    uint64_t physical;
    uint32_t index;
    uint32_t cycle;
};

/* The ring the controller writes and the driver reads. No link entry: it
   wraps, and the driver flips its own idea of the cycle bit when it does. */
struct event_ring {
    struct trb *entries;
    uint64_t physical;
    uint32_t index;
    uint32_t cycle;
};

/* One attached device: its slot, the contexts the controller reads, and the
   ring its control endpoint runs on. */
struct xhci_device {
    int used;
    uint32_t slot;
    uint32_t port;
    uint32_t speed;
    uint32_t packet_size;
    uint64_t input_physical;
    uint32_t *input_context;
    uint64_t output_physical;
    struct producer_ring control_ring;

    /* What the configuration descriptor said, if this turned out to be a HID
       device answering the boot protocol. */
    uint8_t hid_protocol;      /* keyboard, mouse, or zero for neither */
    uint8_t interface_number;
    uint8_t configuration_value;
    uint8_t endpoint_address;
    uint16_t endpoint_packet;
    uint8_t endpoint_interval;

    /* Mass storage, if that is what this turned out to be: the two bulk
       endpoints it moves everything through, each with its own ring. */
    int is_storage;
    uint8_t bulk_in_address;
    uint8_t bulk_out_address;
    uint16_t bulk_in_packet;
    uint16_t bulk_out_packet;
    uint32_t bulk_in_dci;
    uint32_t bulk_out_dci;
    struct producer_ring bulk_in_ring;
    struct producer_ring bulk_out_ring;

    /* The interrupt endpoint, once it is configured and running. */
    uint32_t endpoint_dci;
    struct producer_ring transfer_ring;
    uint8_t *report;
    uint64_t report_physical;
    uint8_t previous[KEYBOARD_REPORT_BYTES];
    int running;
};

static struct xhci_controller controller;
static struct xhci_device devices[MAX_DEVICES];
static struct producer_ring command_ring;
static struct event_ring event_ring;
static uint64_t *device_contexts;      /* the DCBAA */
static uint64_t interrupter;           /* runtime + XHCI_INTERRUPTER0 */

/* One zeroed page of DMA memory, with its physical address. The physical
   allocator only ever hands out memory inside the direct map, so a kernel
   pointer to it is a subtraction away and no extra mapping is needed. */
static void *dma_page(uint64_t *physical_out) {
    void *physical = pmm_alloc_page();
    if (!physical) return NULL;
    void *virtual_address = vmm_phys_to_virt((uint64_t)physical);
    if (!virtual_address) return NULL;
    memset(virtual_address, 0, RING_BYTES);
    *physical_out = (uint64_t)physical;
    return virtual_address;
}

static inline uint32_t mmio_read32(uint64_t address) {
    return *(volatile uint32_t *)address;
}

static inline void mmio_write32(uint64_t address, uint32_t value) {
    *(volatile uint32_t *)address = value;
}

uint32_t xhci_read32(uint64_t address) { return mmio_read32(address); }
void xhci_write32(uint64_t address, uint32_t value) { mmio_write32(address, value); }

uint64_t xhci_read64(uint64_t address) {
    /* Two 32-bit halves rather than one 64-bit access: a controller behind a
       32-bit bridge answers the wide read with garbage in the high word. */
    uint64_t low = mmio_read32(address);
    uint64_t high = mmio_read32(address + 4U);
    return low | (high << 32);
}

void xhci_write64(uint64_t address, uint64_t value) {
    mmio_write32(address, (uint32_t)value);
    mmio_write32(address + 4U, (uint32_t)(value >> 32));
}

struct xhci_controller *xhci_get(void) {
    return controller.present ? &controller : NULL;
}

/* Map the register block uncached. Returns the virtual base, or 0. */
static uint64_t map_registers(uint64_t physical) {
    if (physical & (MMIO_PAGE_BYTES - 1)) return 0;
    if (XHCI_REGISTER_BYTES > DEVICE_MMIO_VIRTUAL_BYTES) return 0;

    uint64_t cr3 = vmm_kernel_cr3();
    for (uint64_t offset = 0; offset < XHCI_REGISTER_BYTES; offset += MMIO_PAGE_BYTES) {
        if (vmm_map_page_in(cr3, DEVICE_MMIO_VIRTUAL_BASE + offset, physical + offset,
                            PAGE_WRITE | PAGE_DEVICE | PAGE_UNCACHED | PAGE_NX) != 0)
            return 0;
    }
    return DEVICE_MMIO_VIRTUAL_BASE;
}

/* Spin until the masked bits reach `wanted`, or the deadline passes. */
static int wait_for(uint64_t address, uint32_t mask, uint32_t wanted, uint64_t timeout_ns) {
    uint64_t deadline = time_uptime_ns() + timeout_ns;
    for (;;) {
        if ((mmio_read32(address) & mask) == wanted) return 0;
        if (time_uptime_ns() >= deadline) return -1;
        __asm__ volatile("pause");
    }
}

/* Stop the controller and put it back to its power-on state. Whatever the
   firmware left running -- and firmware does leave USB running, because it was
   reading a keyboard a moment ago -- is not a state this driver knows. */
static int reset_controller(void) {
    uint64_t operational = controller.operational;

    uint32_t command = mmio_read32(operational + XHCI_USBCMD);
    if (command & USBCMD_RUN) {
        mmio_write32(operational + XHCI_USBCMD, command & ~USBCMD_RUN);
        if (wait_for(operational + XHCI_USBSTS, USBSTS_HALTED, USBSTS_HALTED,
                     XHCI_HALT_TIMEOUT_NS) != 0) {
            kprintf("XHCI: controller will not halt\n");
            return -1;
        }
    }

    mmio_write32(operational + XHCI_USBCMD,
                 mmio_read32(operational + XHCI_USBCMD) | USBCMD_RESET);
    /* The reset bit clears itself when the reset is done, and only then is the
       rest of the register file worth reading. */
    if (wait_for(operational + XHCI_USBCMD, USBCMD_RESET, 0, XHCI_RESET_TIMEOUT_NS) != 0) {
        kprintf("XHCI: reset did not complete\n");
        return -1;
    }
    if (wait_for(operational + XHCI_USBSTS, USBSTS_NOT_READY, 0, XHCI_RESET_TIMEOUT_NS) != 0) {
        kprintf("XHCI: controller stayed not-ready after reset\n");
        return -1;
    }
    return 0;
}

/* Build a command ring: empty, with its last entry pointing back at the
   first. The Toggle Cycle bit on that link is what tells the controller to
   invert its cycle state on the way round, which is how a ring with no end
   marker still has laps. */
static int build_command_ring(void) {
    command_ring.entries = dma_page(&command_ring.physical);
    if (!command_ring.entries) return -1;
    command_ring.index = 0;
    command_ring.cycle = 1;

    struct trb *link = &command_ring.entries[RING_TRB_COUNT - 1];
    link->parameter_low = (uint32_t)command_ring.physical;
    link->parameter_high = (uint32_t)(command_ring.physical >> 32);
    link->status = 0;
    link->control = (TRB_TYPE_LINK << TRB_TYPE_SHIFT) | TRB_TOGGLE_CYCLE;

    xhci_write64(controller.operational + XHCI_CRCR,
                 command_ring.physical | CRCR_RING_CYCLE_STATE);
    return 0;
}

/* The event ring is described to the controller indirectly, through a table of
   segments; this driver uses exactly one segment, so the table has one entry. */
static int build_event_ring(void) {
    event_ring.entries = dma_page(&event_ring.physical);
    if (!event_ring.entries) return -1;
    event_ring.index = 0;
    event_ring.cycle = 1;

    uint64_t table_physical = 0;
    uint32_t *table = dma_page(&table_physical);
    if (!table) return -1;
    table[0] = (uint32_t)event_ring.physical;
    table[1] = (uint32_t)(event_ring.physical >> 32);
    table[2] = RING_TRB_COUNT;
    table[3] = 0;

    /* Size before base: the controller reads the table when the base is
       written, and a table it thinks is zero-length is one it never reads. */
    xhci_write32(interrupter + XHCI_ERSTSZ, EVENT_RING_SEGMENTS);
    xhci_write64(interrupter + XHCI_ERDP, event_ring.physical | ERDP_EVENT_HANDLER_BUSY);
    xhci_write64(interrupter + XHCI_ERSTBA, table_physical);
    return 0;
}

/* Slot contexts live in an array the controller owns; entry zero is reserved
   for the scratchpad table, which is memory the controller asks to borrow. */
static int build_device_contexts(void) {
    uint64_t physical = 0;
    device_contexts = dma_page(&physical);
    if (!device_contexts) return -1;

    uint32_t structural2 = xhci_read32(controller.base + XHCI_HCSPARAMS2);
    uint32_t scratchpads =
        ((structural2 >> HCSPARAMS2_SCRATCHPAD_HIGH_SHIFT) & HCSPARAMS2_SCRATCHPAD_HIGH_MASK) |
        (((structural2 >> HCSPARAMS2_SCRATCHPAD_LOW_SHIFT) & HCSPARAMS2_SCRATCHPAD_LOW_MASK) << 5);

    if (scratchpads) {
        uint64_t table_physical = 0;
        uint64_t *table = dma_page(&table_physical);
        if (!table) return -1;
        if (scratchpads > RING_BYTES / sizeof(uint64_t)) return -1;
        for (uint32_t i = 0; i < scratchpads; i++) {
            uint64_t buffer = 0;
            if (!dma_page(&buffer)) return -1;
            table[i] = buffer;
        }
        device_contexts[0] = table_physical;
    }

    xhci_write32(controller.operational + XHCI_CONFIG, controller.max_slots);
    xhci_write64(controller.operational + XHCI_DCBAAP, physical);
    return 0;
}

/* Put a TRB on the command ring and ring the controller's doorbell. */
static void submit_command(uint32_t type, uint64_t parameter) {
    struct trb *entry = &command_ring.entries[command_ring.index];
    entry->parameter_low = (uint32_t)parameter;
    entry->parameter_high = (uint32_t)(parameter >> 32);
    entry->status = 0;
    /* The cycle bit last: until it is written the controller must not see the
       entry, and everything above has to be in memory before it does. */
    __asm__ volatile("" ::: "memory");
    entry->control = (type << TRB_TYPE_SHIFT) | command_ring.cycle;

    command_ring.index++;
    if (command_ring.index == RING_TRB_COUNT - 1) {
        struct trb *link = &command_ring.entries[RING_TRB_COUNT - 1];
        link->control = (link->control & ~TRB_CYCLE) | command_ring.cycle;
        command_ring.index = 0;
        command_ring.cycle ^= 1;
    }

    __asm__ volatile("" ::: "memory");
    xhci_write32(controller.doorbell, 0);
}

/* Take the next event the controller has posted, or nothing. */
static int next_event(struct trb *out) {
    struct trb *entry = &event_ring.entries[event_ring.index];
    if ((entry->control & TRB_CYCLE) != event_ring.cycle) return 0;

    *out = *entry;
    event_ring.index++;
    if (event_ring.index == RING_TRB_COUNT) {
        event_ring.index = 0;
        event_ring.cycle ^= 1;
    }
    xhci_write64(interrupter + XHCI_ERDP,
                 (event_ring.physical + (uint64_t)event_ring.index * TRB_BYTES) |
                 ERDP_EVENT_HANDLER_BUSY);
    return 1;
}

/* Wait for the completion of the command just submitted. Port status changes
   arrive on the same ring and are simply stepped over here; enumeration reads
   the ports directly rather than relying on having caught the event. */
static int wait_for_command(uint32_t *completion_code) {
    uint64_t deadline = time_uptime_ns() + COMMAND_TIMEOUT_NS;
    for (;;) {
        struct trb event;
        if (next_event(&event)) {
            uint32_t type = (event.control >> TRB_TYPE_SHIFT) & TRB_TYPE_MASK;
            if (type == TRB_TYPE_COMMAND_COMPLETION) {
                *completion_code = (event.status >> TRB_COMPLETION_SHIFT) & TRB_COMPLETION_MASK;
                return 0;
            }
            if (type == TRB_TYPE_PORT_STATUS_CHANGE) continue;
        }
        if (time_uptime_ns() >= deadline) return -1;
        __asm__ volatile("pause");
    }
}

/* The command completion event also carries the slot the controller assigned,
   which is the only way to learn it. */
static int run_command(uint32_t type, uint64_t parameter, uint32_t control_extra,
                       uint32_t *slot_out) {
    struct trb *entry = &command_ring.entries[command_ring.index];
    entry->parameter_low = (uint32_t)parameter;
    entry->parameter_high = (uint32_t)(parameter >> 32);
    entry->status = 0;
    __asm__ volatile("" ::: "memory");
    entry->control = (type << TRB_TYPE_SHIFT) | control_extra | command_ring.cycle;

    command_ring.index++;
    if (command_ring.index == RING_TRB_COUNT - 1) {
        struct trb *link = &command_ring.entries[RING_TRB_COUNT - 1];
        link->control = (link->control & ~TRB_CYCLE) | command_ring.cycle;
        command_ring.index = 0;
        command_ring.cycle ^= 1;
    }
    __asm__ volatile("" ::: "memory");
    xhci_write32(controller.doorbell, 0);

    uint64_t deadline = time_uptime_ns() + COMMAND_TIMEOUT_NS;
    for (;;) {
        struct trb event;
        if (next_event(&event)) {
            uint32_t event_type = (event.control >> TRB_TYPE_SHIFT) & TRB_TYPE_MASK;
            if (event_type == TRB_TYPE_COMMAND_COMPLETION) {
                if (slot_out) *slot_out = (event.control >> COMMAND_SLOT_SHIFT) & 0xFFU;
                uint32_t code = (event.status >> TRB_COMPLETION_SHIFT) & TRB_COMPLETION_MASK;
                return code == TRB_COMPLETION_SUCCESS ? 0 : -(int)code;
            }
        }
        if (time_uptime_ns() >= deadline) return -1;
        __asm__ volatile("pause");
    }
}

static uint64_t port_register(uint32_t port) {
    return controller.operational + XHCI_PORTSC_BASE + (uint64_t)(port - 1) * XHCI_PORT_STRIDE;
}

/* Drive a port through reset and report the speed the controller then sees.
   USB 3 ports enable themselves on connect; USB 2 ports need the reset, and
   the keyboard is on one of those. */
static int reset_port(uint32_t port, uint32_t *speed_out) {
    uint64_t address = port_register(port);
    uint32_t status = xhci_read32(address);
    if (!(status & PORTSC_CONNECTED)) return -1;

    if (!(status & PORTSC_ENABLED)) {
        xhci_write32(address, (status & PORTSC_PRESERVE_MASK) | PORTSC_RESET);
        uint64_t deadline = time_uptime_ns() + PORT_RESET_TIMEOUT_NS;
        for (;;) {
            status = xhci_read32(address);
            if (status & PORTSC_ENABLED) break;
            if (time_uptime_ns() >= deadline) return -1;
            __asm__ volatile("pause");
        }
        uint64_t settle = time_uptime_ns() + PORT_SETTLE_NS;
        while (time_uptime_ns() < settle) __asm__ volatile("pause");
    }

    /* Acknowledge whatever changed, without disturbing the rest. */
    status = xhci_read32(address);
    xhci_write32(address, (status & PORTSC_PRESERVE_MASK) | (status & PORTSC_CHANGE_MASK));

    *speed_out = (status >> PORTSC_SPEED_SHIFT) & PORTSC_SPEED_MASK;
    return 0;
}

static uint32_t packet_size_for(uint32_t speed) {
    switch (speed) {
        case USB_SPEED_SUPER: return EP0_PACKET_SUPER;
        case USB_SPEED_HIGH: return EP0_PACKET_HIGH;
        default: return EP0_PACKET_LOW_FULL;
    }
}

static uint32_t *context_at(uint32_t *base, uint32_t index) {
    return base + (index * controller.context_bytes) / sizeof(uint32_t);
}

/* Describe the device to the controller and give it an address. The input
   context says what to add -- the slot and its control endpoint -- and the
   controller writes what it made of it into the output context. */
static int address_device(struct xhci_device *device) {
    uint64_t input_physical = 0;
    device->input_context = dma_page(&input_physical);
    if (!device->input_context) return -1;
    device->input_physical = input_physical;

    uint64_t output_physical = 0;
    if (!dma_page(&output_physical)) return -1;
    device->output_physical = output_physical;

    device->control_ring.entries = dma_page(&device->control_ring.physical);
    if (!device->control_ring.entries) return -1;
    device->control_ring.index = 0;
    device->control_ring.cycle = 1;
    struct trb *link = &device->control_ring.entries[RING_TRB_COUNT - 1];
    link->parameter_low = (uint32_t)device->control_ring.physical;
    link->parameter_high = (uint32_t)(device->control_ring.physical >> 32);
    link->control = (TRB_TYPE_LINK << TRB_TYPE_SHIFT) | TRB_TOGGLE_CYCLE;

    uint32_t *control = context_at(device->input_context, INPUT_CONTROL_INDEX);
    control[1] = INPUT_ADD_SLOT | INPUT_ADD_EP0;

    uint32_t *slot = context_at(device->input_context, SLOT_CONTEXT_INDEX);
    slot[0] = (device->speed << SLOT_SPEED_SHIFT) | (1U << SLOT_CONTEXT_ENTRIES_SHIFT);
    slot[1] = device->port << SLOT_ROOT_PORT_SHIFT;

    uint32_t *endpoint = context_at(device->input_context, EP0_CONTEXT_INDEX);
    endpoint[1] = (EP_TYPE_CONTROL << EP_TYPE_SHIFT) |
                  (EP_ERROR_COUNT << EP_ERROR_COUNT_SHIFT) |
                  (device->packet_size << EP_MAX_PACKET_SHIFT);
    endpoint[2] = (uint32_t)(device->control_ring.physical | EP_DEQUEUE_CYCLE);
    endpoint[3] = (uint32_t)(device->control_ring.physical >> 32);

    device_contexts[device->slot] = device->output_physical;

    return run_command(TRB_TYPE_ADDRESS_DEVICE, input_physical,
                       device->slot << COMMAND_SLOT_SHIFT, NULL);
}

/* Put one TRB on a device's own ring. Same cycle-bit dance as the command
   ring; the difference is which doorbell wakes it. */
static void enqueue(struct producer_ring *ring, uint64_t parameter,
                    uint32_t status, uint32_t type, uint32_t control_extra) {
    struct trb *entry = &ring->entries[ring->index];
    entry->parameter_low = (uint32_t)parameter;
    entry->parameter_high = (uint32_t)(parameter >> 32);
    entry->status = status;
    __asm__ volatile("" ::: "memory");
    entry->control = (type << TRB_TYPE_SHIFT) | control_extra | ring->cycle;

    ring->index++;
    if (ring->index == RING_TRB_COUNT - 1) {
        struct trb *link = &ring->entries[RING_TRB_COUNT - 1];
        link->control = (link->control & ~TRB_CYCLE) | ring->cycle;
        ring->index = 0;
        ring->cycle ^= 1;
    }
}

static void ring_doorbell(uint32_t slot, uint32_t target) {
    __asm__ volatile("" ::: "memory");
    xhci_write32(controller.doorbell + (uint64_t)slot * DOORBELL_STRIDE, target);
}

static int wait_for_transfer(void) {
    uint64_t deadline = time_uptime_ns() + TRANSFER_TIMEOUT_NS;
    for (;;) {
        struct trb event;
        if (next_event(&event)) {
            uint32_t type = (event.control >> TRB_TYPE_SHIFT) & TRB_TYPE_MASK;
            if (type == TRB_TYPE_TRANSFER_EVENT) {
                uint32_t code = (event.status >> TRB_COMPLETION_SHIFT) & TRB_COMPLETION_MASK;
                /* A short packet is how a device says "that is all I have",
                   which for a descriptor read is success, not failure. */
                return (code == TRB_COMPLETION_SUCCESS || code == 13U) ? 0 : -(int)code;
            }
        }
        if (time_uptime_ns() >= deadline) return -1;
        __asm__ volatile("pause");
    }
}

/* A control transfer: setup, an optional data stage, then a status stage the
   other way round. The interrupt-on-completion bit goes on the last stage,
   because that is the one whose event says the whole thing is done. */
static int control_transfer(struct xhci_device *device, uint8_t request_type,
                            uint8_t request, uint16_t value, uint16_t index,
                            void *buffer, uint16_t length) {
    uint64_t setup = (uint64_t)request_type | ((uint64_t)request << 8) |
                     ((uint64_t)value << 16) | ((uint64_t)index << 32) |
                     ((uint64_t)length << 48);

    uint32_t transfer_type = TRB_TRANSFER_TYPE_NO_DATA;
    if (length) {
        transfer_type = (request_type & USB_DIRECTION_IN) ? TRB_TRANSFER_TYPE_IN
                                                          : TRB_TRANSFER_TYPE_OUT;
    }

    enqueue(&device->control_ring, setup, SETUP_PACKET_BYTES, TRB_TYPE_SETUP_STAGE,
            TRB_IMMEDIATE_DATA | (transfer_type << TRB_TRANSFER_TYPE_SHIFT));

    if (length) {
        uint64_t physical = vmm_virt_to_phys_direct(buffer);
        enqueue(&device->control_ring, physical, length, TRB_TYPE_DATA_STAGE,
                (request_type & USB_DIRECTION_IN) ? TRB_DIRECTION_IN : 0);
    }

    /* The status stage runs opposite to the data, and with no data it is IN. */
    uint32_t status_direction = (length && (request_type & USB_DIRECTION_IN)) ? 0 : TRB_DIRECTION_IN;
    enqueue(&device->control_ring, 0, 0, TRB_TYPE_STATUS_STAGE,
            status_direction | TRB_INTERRUPT_ON_COMPLETION);

    ring_doorbell(device->slot, EP0_DOORBELL_TARGET);
    return wait_for_transfer();
}

/* The first thing anyone asks a USB device. Its answer includes the real
   maximum packet size for endpoint zero, which the addressing step could only
   guess at from the port speed. */
static int read_device_descriptor(struct xhci_device *device, uint8_t *out) {
    return control_transfer(device, USB_DIRECTION_IN, USB_REQUEST_GET_DESCRIPTOR,
                            USB_DESCRIPTOR_DEVICE << USB_DESCRIPTOR_TYPE_SHIFT, 0,
                            out, USB_DEVICE_DESCRIPTOR_BYTES);
}

/* Read the configuration descriptor and pick out a boot-protocol HID
 * interface and the interrupt endpoint it reports on.
 *
 * The descriptor is a flat run of variable-length records, so it is walked by
 * length rather than indexed. Only the endpoint that follows the interface we
 * accepted is taken, which is what keeps a composite device's other interfaces
 * from being mistaken for the keyboard's.
 */
static int find_hid_interface(struct xhci_device *device, uint8_t *buffer) {
    if (control_transfer(device, USB_DIRECTION_IN, USB_REQUEST_GET_DESCRIPTOR,
                         USB_DESCRIPTOR_CONFIGURATION << USB_DESCRIPTOR_TYPE_SHIFT, 0,
                         buffer, CONFIGURATION_DESCRIPTOR_BYTES) != 0) return -1;

    uint16_t total = (uint16_t)(buffer[CONFIGURATION_TOTAL_LENGTH_OFFSET] |
                                (buffer[CONFIGURATION_TOTAL_LENGTH_OFFSET + 1] << 8));
    if (!total || total > RING_BYTES) return -1;
    if (control_transfer(device, USB_DIRECTION_IN, USB_REQUEST_GET_DESCRIPTOR,
                         USB_DESCRIPTOR_CONFIGURATION << USB_DESCRIPTOR_TYPE_SHIFT, 0,
                         buffer, total) != 0) return -1;

    device->configuration_value = buffer[CONFIGURATION_VALUE_OFFSET];

    int in_boot_interface = 0;
    for (uint16_t offset = 0; offset + 2U <= total; ) {
        uint8_t length = buffer[offset + DESCRIPTOR_LENGTH_OFFSET];
        uint8_t type = buffer[offset + DESCRIPTOR_TYPE_OFFSET];
        if (!length || offset + length > total) break;

        if (type == USB_DESCRIPTOR_INTERFACE) {
            uint8_t class_code = buffer[offset + INTERFACE_CLASS_OFFSET];
            uint8_t subclass = buffer[offset + INTERFACE_SUBCLASS_OFFSET];
            uint8_t protocol = buffer[offset + INTERFACE_PROTOCOL_OFFSET];
            in_boot_interface = class_code == USB_CLASS_HID &&
                                subclass == HID_SUBCLASS_BOOT &&
                                (protocol == HID_PROTOCOL_KEYBOARD ||
                                 protocol == HID_PROTOCOL_MOUSE);
            if (in_boot_interface) {
                device->hid_protocol = protocol;
                device->interface_number = buffer[offset + INTERFACE_NUMBER_OFFSET];
            }
        } else if (type == USB_DESCRIPTOR_ENDPOINT && in_boot_interface) {
            uint8_t address = buffer[offset + ENDPOINT_ADDRESS_OFFSET];
            uint8_t attributes = buffer[offset + ENDPOINT_ATTRIBUTES_OFFSET];
            if ((address & ENDPOINT_DIRECTION_IN) &&
                (attributes & ENDPOINT_TYPE_MASK) == ENDPOINT_TYPE_INTERRUPT) {
                device->endpoint_address = address;
                device->endpoint_packet =
                    (uint16_t)(buffer[offset + ENDPOINT_MAX_PACKET_OFFSET] |
                               (buffer[offset + ENDPOINT_MAX_PACKET_OFFSET + 1] << 8));
                device->endpoint_interval = buffer[offset + ENDPOINT_INTERVAL_OFFSET];
                return 0;
            }
        }
        offset = (uint16_t)(offset + length);
    }

    device->hid_protocol = 0;
    return -1;
}

/* HID usage to keycode. Only the boot-protocol range is here, because that is
   the only range a boot-protocol device is allowed to send. The layout is the
   usage table's own order -- letters, digits, then the rest -- which is why
   this is a switch over ranges rather than a flat array. */
static uint16_t keycode_for_usage(uint8_t usage) {
    static const uint16_t letters[] = {
        TUNIX_KEY_A, TUNIX_KEY_B, TUNIX_KEY_C, TUNIX_KEY_D, TUNIX_KEY_E,
        TUNIX_KEY_F, TUNIX_KEY_G, TUNIX_KEY_H, TUNIX_KEY_I, TUNIX_KEY_J,
        TUNIX_KEY_K, TUNIX_KEY_L, TUNIX_KEY_M, TUNIX_KEY_N, TUNIX_KEY_O,
        TUNIX_KEY_P, TUNIX_KEY_Q, TUNIX_KEY_R, TUNIX_KEY_S, TUNIX_KEY_T,
        TUNIX_KEY_U, TUNIX_KEY_V, TUNIX_KEY_W, TUNIX_KEY_X, TUNIX_KEY_Y,
        TUNIX_KEY_Z,
    };
    static const uint16_t digits[] = {
        TUNIX_KEY_1, TUNIX_KEY_2, TUNIX_KEY_3, TUNIX_KEY_4, TUNIX_KEY_5,
        TUNIX_KEY_6, TUNIX_KEY_7, TUNIX_KEY_8, TUNIX_KEY_9, TUNIX_KEY_0,
    };
    static const uint16_t punctuation[] = {
        TUNIX_KEY_ENTER, TUNIX_KEY_ESC, TUNIX_KEY_BACKSPACE, TUNIX_KEY_TAB,
        TUNIX_KEY_SPACE, TUNIX_KEY_MINUS, TUNIX_KEY_EQUAL, TUNIX_KEY_LEFTBRACE,
        TUNIX_KEY_RIGHTBRACE, TUNIX_KEY_BACKSLASH, TUNIX_KEY_RESERVED,
        TUNIX_KEY_SEMICOLON, TUNIX_KEY_APOSTROPHE, TUNIX_KEY_GRAVE,
        TUNIX_KEY_COMMA, TUNIX_KEY_DOT, TUNIX_KEY_SLASH, TUNIX_KEY_CAPSLOCK,
    };
    static const uint16_t function_keys[] = {
        TUNIX_KEY_F1, TUNIX_KEY_F2, TUNIX_KEY_F3, TUNIX_KEY_F4, TUNIX_KEY_F5,
        TUNIX_KEY_F6, TUNIX_KEY_F7, TUNIX_KEY_F8, TUNIX_KEY_F9, TUNIX_KEY_F10,
        TUNIX_KEY_F11, TUNIX_KEY_F12,
    };

    if (usage >= 0x04U && usage <= 0x1DU) return letters[usage - 0x04U];
    if (usage >= 0x1EU && usage <= 0x27U) return digits[usage - 0x1EU];
    if (usage >= 0x28U && usage <= 0x39U) return punctuation[usage - 0x28U];
    if (usage >= 0x3AU && usage <= 0x45U) return function_keys[usage - 0x3AU];
    switch (usage) {
        case 0x46U: return TUNIX_KEY_SYSRQ;
        case 0x47U: return TUNIX_KEY_SCROLLLOCK;
        case 0x48U: return TUNIX_KEY_PAUSE;
        case 0x49U: return TUNIX_KEY_INSERT;
        case 0x4AU: return TUNIX_KEY_HOME;
        case 0x4BU: return TUNIX_KEY_PAGEUP;
        case 0x4CU: return TUNIX_KEY_DELETE;
        case 0x4DU: return TUNIX_KEY_END;
        case 0x4EU: return TUNIX_KEY_PAGEDOWN;
        case 0x4FU: return TUNIX_KEY_RIGHT;
        case 0x50U: return TUNIX_KEY_LEFT;
        case 0x51U: return TUNIX_KEY_DOWN;
        case 0x52U: return TUNIX_KEY_UP;
        case 0x53U: return TUNIX_KEY_NUMLOCK;
        default: return TUNIX_KEY_RESERVED;
    }
}

static uint16_t keycode_for_modifier(unsigned bit) {
    static const uint16_t modifiers[KEYBOARD_MODIFIER_COUNT] = {
        TUNIX_KEY_LEFTCTRL, TUNIX_KEY_LEFTSHIFT, TUNIX_KEY_LEFTALT,
        TUNIX_KEY_LEFTMETA, TUNIX_KEY_RIGHTCTRL, TUNIX_KEY_RIGHTSHIFT,
        TUNIX_KEY_RIGHTALT, TUNIX_KEY_RIGHTMETA,
    };
    return modifiers[bit];
}

/* A boot keyboard reports the set of keys currently held, not the change. The
   difference against the previous report is where presses and releases come
   from, in both directions. */
static void handle_keyboard_report(struct xhci_device *device) {
    const uint8_t *now = device->report;
    const uint8_t *before = device->previous;

    for (unsigned bit = 0; bit < KEYBOARD_MODIFIER_COUNT; bit++) {
        uint8_t mask = (uint8_t)(1U << bit);
        if ((now[0] & mask) == (before[0] & mask)) continue;
        input_external_key(keycode_for_modifier(bit), (now[0] & mask) ? 0 : 1);
    }

    for (unsigned i = 0; i < KEYBOARD_REPORT_KEYS; i++) {
        uint8_t usage = before[KEYBOARD_REPORT_KEYS_OFFSET + i];
        if (usage < HID_USAGE_FIRST_KEY) continue;
        int still_held = 0;
        for (unsigned j = 0; j < KEYBOARD_REPORT_KEYS; j++)
            if (now[KEYBOARD_REPORT_KEYS_OFFSET + j] == usage) still_held = 1;
        if (!still_held) input_external_key(keycode_for_usage(usage), 1);
    }

    for (unsigned i = 0; i < KEYBOARD_REPORT_KEYS; i++) {
        uint8_t usage = now[KEYBOARD_REPORT_KEYS_OFFSET + i];
        if (usage < HID_USAGE_FIRST_KEY) continue;
        int was_held = 0;
        for (unsigned j = 0; j < KEYBOARD_REPORT_KEYS; j++)
            if (before[KEYBOARD_REPORT_KEYS_OFFSET + j] == usage) was_held = 1;
        if (!was_held) input_external_key(keycode_for_usage(usage), 0);
    }

    memcpy(device->previous, now, KEYBOARD_REPORT_BYTES);
}

/* A boot mouse reports movement since the last report, so it needs no memory
   of what came before. */
static void handle_mouse_report(struct xhci_device *device) {
    const uint8_t *report = device->report;
    input_external_mouse((int)(int8_t)report[1], (int)(int8_t)report[2],
                         (int)(int8_t)report[3], report[0] & MOUSE_BUTTON_MASK);
}

/* Hand the endpoint a buffer and tell it to fill it. Every completed report
   costs one of these, which is why it is also called from the poll. */
static void arm_transfer(struct xhci_device *device) {
    enqueue(&device->transfer_ring, device->report_physical, device->endpoint_packet,
            TRB_TYPE_NORMAL, TRB_INTERRUPT_ON_COMPLETION);
    ring_doorbell(device->slot, device->endpoint_dci);
}

/* Describe the interrupt endpoint to the controller and turn it on. The input
   context adds one endpoint and restates the slot, because the slot's count of
   how many endpoint contexts are valid has to grow with it. */
static int configure_endpoint(struct xhci_device *device) {
    uint8_t number = device->endpoint_address & ENDPOINT_NUMBER_MASK;
    device->endpoint_dci = (uint32_t)number * 2U + 1U;

    device->transfer_ring.entries = dma_page(&device->transfer_ring.physical);
    device->report = dma_page(&device->report_physical);
    if (!device->transfer_ring.entries || !device->report) return -1;
    device->transfer_ring.index = 0;
    device->transfer_ring.cycle = 1;
    struct trb *link = &device->transfer_ring.entries[RING_TRB_COUNT - 1];
    link->parameter_low = (uint32_t)device->transfer_ring.physical;
    link->parameter_high = (uint32_t)(device->transfer_ring.physical >> 32);
    link->control = (TRB_TYPE_LINK << TRB_TYPE_SHIFT) | TRB_TOGGLE_CYCLE;

    /* The interval field is a power of two in microframes. High speed already
       states it that way; full and low speed count whole frames instead. */
    uint32_t interval = device->endpoint_interval ? device->endpoint_interval - 1U : 0U;
    if (device->speed == USB_SPEED_LOW || device->speed == USB_SPEED_FULL) {
        uint32_t microframes = (uint32_t)device->endpoint_interval * MICROFRAMES_PER_FRAME;
        interval = 0;
        while ((1U << (interval + 1U)) <= microframes && interval < EP_MAX_INTERVAL) interval++;
    }
    if (interval > EP_MAX_INTERVAL) interval = EP_MAX_INTERVAL;

    memset(device->input_context, 0, RING_BYTES);
    uint32_t *control = context_at(device->input_context, INPUT_CONTROL_INDEX);
    control[1] = INPUT_ADD_SLOT | (1U << device->endpoint_dci);

    uint32_t *slot = context_at(device->input_context, SLOT_CONTEXT_INDEX);
    slot[0] = (device->speed << SLOT_SPEED_SHIFT) |
              (device->endpoint_dci << SLOT_CONTEXT_ENTRIES_SHIFT);
    slot[1] = device->port << SLOT_ROOT_PORT_SHIFT;

    uint32_t *endpoint = context_at(device->input_context, device->endpoint_dci + 1U);
    endpoint[0] = interval << EP_INTERVAL_SHIFT;
    endpoint[1] = (EP_TYPE_INTERRUPT_IN << EP_TYPE_SHIFT) |
                  (EP_ERROR_COUNT << EP_ERROR_COUNT_SHIFT) |
                  ((uint32_t)device->endpoint_packet << EP_MAX_PACKET_SHIFT);
    endpoint[2] = (uint32_t)(device->transfer_ring.physical | EP_DEQUEUE_CYCLE);
    endpoint[3] = (uint32_t)(device->transfer_ring.physical >> 32);
    endpoint[4] = ((uint32_t)device->endpoint_packet << EP_AVERAGE_TRB_SHIFT) |
                  ((uint32_t)device->endpoint_packet << EP_MAX_ESIT_SHIFT);

    return run_command(TRB_TYPE_CONFIGURE_ENDPOINT, device->input_physical,
                       device->slot << COMMAND_SLOT_SHIFT, NULL);
}

/*
 * Look for a bulk-only mass-storage interface and both of its endpoints. The
 * walk is the same shape as the HID one; what differs is that two endpoints
 * are wanted rather than one, and a device is only usable when both arrive.
 */
static int find_storage_interface(struct xhci_device *device, uint8_t *buffer) {
    if (control_transfer(device, USB_DIRECTION_IN, USB_REQUEST_GET_DESCRIPTOR,
                         USB_DESCRIPTOR_CONFIGURATION << USB_DESCRIPTOR_TYPE_SHIFT, 0,
                         buffer, CONFIGURATION_DESCRIPTOR_BYTES) != 0) return -1;
    uint16_t total = (uint16_t)(buffer[CONFIGURATION_TOTAL_LENGTH_OFFSET] |
                                (buffer[CONFIGURATION_TOTAL_LENGTH_OFFSET + 1] << 8));
    if (!total || total > RING_BYTES) return -1;
    if (control_transfer(device, USB_DIRECTION_IN, USB_REQUEST_GET_DESCRIPTOR,
                         USB_DESCRIPTOR_CONFIGURATION << USB_DESCRIPTOR_TYPE_SHIFT, 0,
                         buffer, total) != 0) return -1;

    device->configuration_value = buffer[CONFIGURATION_VALUE_OFFSET];
    device->bulk_in_address = 0;
    device->bulk_out_address = 0;

    int in_storage = 0;
    for (uint16_t offset = 0; offset + 2U <= total; ) {
        uint8_t length = buffer[offset + DESCRIPTOR_LENGTH_OFFSET];
        uint8_t type = buffer[offset + DESCRIPTOR_TYPE_OFFSET];
        if (!length || offset + length > total) break;

        if (type == USB_DESCRIPTOR_INTERFACE) {
            in_storage = buffer[offset + INTERFACE_CLASS_OFFSET] == USB_CLASS_MASS_STORAGE &&
                         buffer[offset + INTERFACE_SUBCLASS_OFFSET] == MSC_SUBCLASS_SCSI &&
                         buffer[offset + INTERFACE_PROTOCOL_OFFSET] == MSC_PROTOCOL_BULK_ONLY;
            if (in_storage) device->interface_number = buffer[offset + INTERFACE_NUMBER_OFFSET];
        } else if (type == USB_DESCRIPTOR_ENDPOINT && in_storage) {
            uint8_t address = buffer[offset + ENDPOINT_ADDRESS_OFFSET];
            uint8_t attributes = buffer[offset + ENDPOINT_ATTRIBUTES_OFFSET];
            uint16_t packet = (uint16_t)(buffer[offset + ENDPOINT_MAX_PACKET_OFFSET] |
                                         (buffer[offset + ENDPOINT_MAX_PACKET_OFFSET + 1] << 8));
            if ((attributes & ENDPOINT_TYPE_MASK) == ENDPOINT_TYPE_BULK) {
                if (address & ENDPOINT_DIRECTION_IN) {
                    device->bulk_in_address = address;
                    device->bulk_in_packet = packet;
                } else {
                    device->bulk_out_address = address;
                    device->bulk_out_packet = packet;
                }
            }
        }
        offset = (uint16_t)(offset + length);
    }
    return (device->bulk_in_address && device->bulk_out_address) ? 0 : -1;
}

/* Give a ring its page and its trailing link back to the start. */
static int prepare_ring(struct producer_ring *ring) {
    ring->entries = dma_page(&ring->physical);
    if (!ring->entries) return -1;
    ring->index = 0;
    ring->cycle = 1;
    struct trb *link = &ring->entries[RING_TRB_COUNT - 1];
    link->parameter_low = (uint32_t)ring->physical;
    link->parameter_high = (uint32_t)(ring->physical >> 32);
    link->control = (TRB_TYPE_LINK << TRB_TYPE_SHIFT) | TRB_TOGGLE_CYCLE;
    return 0;
}

/*
 * Both bulk endpoints in one CONFIGURE_ENDPOINT. They have to be added
 * together: the slot's context-entries field states the highest endpoint that
 * is valid, so configuring them one at a time would have the second command
 * describe a slot the first had already sized differently.
 */
static int configure_bulk_endpoints(struct xhci_device *device) {
    device->bulk_in_dci = (uint32_t)(device->bulk_in_address & ENDPOINT_NUMBER_MASK) * 2U + 1U;
    device->bulk_out_dci = (uint32_t)(device->bulk_out_address & ENDPOINT_NUMBER_MASK) * 2U;
    if (prepare_ring(&device->bulk_in_ring) != 0) return -1;
    if (prepare_ring(&device->bulk_out_ring) != 0) return -1;

    uint32_t highest = device->bulk_in_dci > device->bulk_out_dci
        ? device->bulk_in_dci : device->bulk_out_dci;

    memset(device->input_context, 0, RING_BYTES);
    uint32_t *control = context_at(device->input_context, INPUT_CONTROL_INDEX);
    control[1] = INPUT_ADD_SLOT | (1U << device->bulk_in_dci) | (1U << device->bulk_out_dci);

    uint32_t *slot = context_at(device->input_context, SLOT_CONTEXT_INDEX);
    slot[0] = (device->speed << SLOT_SPEED_SHIFT) | (highest << SLOT_CONTEXT_ENTRIES_SHIFT);
    slot[1] = device->port << SLOT_ROOT_PORT_SHIFT;

    uint32_t *in_endpoint = context_at(device->input_context, device->bulk_in_dci + 1U);
    in_endpoint[1] = (EP_TYPE_BULK_IN << EP_TYPE_SHIFT) |
                     (EP_ERROR_COUNT << EP_ERROR_COUNT_SHIFT) |
                     ((uint32_t)device->bulk_in_packet << EP_MAX_PACKET_SHIFT);
    in_endpoint[2] = (uint32_t)(device->bulk_in_ring.physical | EP_DEQUEUE_CYCLE);
    in_endpoint[3] = (uint32_t)(device->bulk_in_ring.physical >> 32);
    in_endpoint[4] = (uint32_t)device->bulk_in_packet << EP_AVERAGE_TRB_SHIFT;

    uint32_t *out_endpoint = context_at(device->input_context, device->bulk_out_dci + 1U);
    out_endpoint[1] = (EP_TYPE_BULK_OUT << EP_TYPE_SHIFT) |
                      (EP_ERROR_COUNT << EP_ERROR_COUNT_SHIFT) |
                      ((uint32_t)device->bulk_out_packet << EP_MAX_PACKET_SHIFT);
    out_endpoint[2] = (uint32_t)(device->bulk_out_ring.physical | EP_DEQUEUE_CYCLE);
    out_endpoint[3] = (uint32_t)(device->bulk_out_ring.physical >> 32);
    out_endpoint[4] = (uint32_t)device->bulk_out_packet << EP_AVERAGE_TRB_SHIFT;

    return run_command(TRB_TYPE_CONFIGURE_ENDPOINT, device->input_physical,
                       device->slot << COMMAND_SLOT_SHIFT, NULL);
}

static int start_storage(struct xhci_device *device) {
    if (control_transfer(device, 0, USB_REQUEST_SET_CONFIGURATION,
                         device->configuration_value, 0, NULL, 0) != 0) return -1;
    if (configure_bulk_endpoints(device) != 0) return -1;
    device->is_storage = 1;
    device->running = 1;
    return 0;
}

/* Everything between "a HID interface was found" and "reports are arriving". */
static int start_hid(struct xhci_device *device) {
    if (control_transfer(device, 0, USB_REQUEST_SET_CONFIGURATION,
                         device->configuration_value, 0, NULL, 0) != 0) return -1;
    if (configure_endpoint(device) != 0) return -1;
    /* Not fatal if refused: a device that only speaks the boot protocol has
       nothing to switch, and answers the request with a stall. */
    (void)control_transfer(device, HID_REQUEST_TYPE_OUT, HID_REQUEST_SET_PROTOCOL,
                           HID_PROTOCOL_BOOT, device->interface_number, NULL, 0);
    device->running = 1;
    arm_transfer(device);
    return 0;
}

static struct xhci_device *device_for_slot(uint32_t slot) {
    for (unsigned i = 0; i < MAX_DEVICES; i++)
        if (devices[i].used && devices[i].running && devices[i].slot == slot)
            return &devices[i];
    return NULL;
}

/* What was found on the bus. Asked once, when the device nodes are created:
   a machine whose keyboard and pointer are on USB has no PS/2 pair to answer
   for them, and an evdev node that is never created is a desktop with no
   mouse. `running` rather than `used`, so a device that enumerated but never
   started reporting does not count. */
static int hid_device_present(uint8_t protocol) {
    if (!controller.present) return 0;
    for (unsigned index = 0; index < MAX_DEVICES; index++) {
        if (devices[index].used && devices[index].running &&
            devices[index].hid_protocol == protocol)
            return 1;
    }
    return 0;
}

int xhci_keyboard_present(void) { return hid_device_present(HID_PROTOCOL_KEYBOARD); }
int xhci_pointer_present(void) { return hid_device_present(HID_PROTOCOL_MOUSE); }

/* Called from the input layer's poll: take whatever the endpoints have
   finished, turn it into key and pointer events, and ask for more. */
void xhci_poll(void) {
    if (!controller.present) return;

    struct trb event;
    while (next_event(&event)) {
        uint32_t type = (event.control >> TRB_TYPE_SHIFT) & TRB_TYPE_MASK;
        if (type != TRB_TYPE_TRANSFER_EVENT) continue;

        struct xhci_device *device =
            device_for_slot((event.control >> COMMAND_SLOT_SHIFT) & 0xFFU);
        if (!device) continue;

        uint32_t code = (event.status >> TRB_COMPLETION_SHIFT) & TRB_COMPLETION_MASK;
        if (code == TRB_COMPLETION_SUCCESS || code == 13U) {
            if (device->hid_protocol == HID_PROTOCOL_KEYBOARD) handle_keyboard_report(device);
            else handle_mouse_report(device);
        }
        arm_transfer(device);
    }
}

/* Walk the root hub and bring up whatever is plugged in. */
static void enumerate_ports(void) {
    unsigned found = 0;
    for (uint32_t port = 1; port <= controller.max_ports && found < MAX_DEVICES; port++) {
        uint32_t status = xhci_read32(port_register(port));
        if (!(status & PORTSC_CONNECTED)) continue;

        uint32_t speed = 0;
        if (reset_port(port, &speed) != 0) {
            kprintf("XHCI: port %u would not reset\n", (unsigned)port);
            continue;
        }

        struct xhci_device *device = &devices[found];
        uint32_t slot = 0;
        if (run_command(TRB_TYPE_ENABLE_SLOT, 0, 0, &slot) != 0 || !slot) {
            kprintf("XHCI: port %u connected but no slot was given\n", (unsigned)port);
            continue;
        }

        device->slot = slot;
        device->port = port;
        device->speed = speed;
        device->packet_size = packet_size_for(speed);
        if (address_device(device) != 0) {
            kprintf("XHCI: port %u slot %u would not take an address\n",
                    (unsigned)port, (unsigned)slot);
            continue;
        }

        device->used = 1;
        found++;

        uint64_t descriptor_physical = 0;
        uint8_t *descriptor = dma_page(&descriptor_physical);
        if (!descriptor || read_device_descriptor(device, descriptor) != 0) {
            kprintf("XHCI: port %u slot %u addressed, but would not describe itself\n",
                    (unsigned)port, (unsigned)slot);
            continue;
        }

        if (find_hid_interface(device, descriptor) != 0) {
            /* Not a keyboard or a mouse; the other thing this driver knows how
               to be is a disk. */
            if (find_storage_interface(device, descriptor) == 0) {
                if (start_storage(device) != 0)
                    kprintf("XHCI: port %u storage did not start\n", (unsigned)port);
                else
                    kprintf("XHCI: port %u slot %u is a mass storage device\n",
                            (unsigned)port, (unsigned)slot);
                found++;
                continue;
            }
            kprintf("XHCI: port %u slot %u is not a device this driver knows\n",
                    (unsigned)port, (unsigned)slot);
            continue;
        }

        if (start_hid(device) != 0) {
            kprintf("XHCI: port %u slot %u would not start reporting\n",
                    (unsigned)port, (unsigned)slot);
            continue;
        }

        kprintf("XHCI: %s on port %u slot %u, endpoint %x reporting\n",
                device->hid_protocol == HID_PROTOCOL_KEYBOARD ? "keyboard" : "mouse",
                (unsigned)port, (unsigned)slot, (unsigned)device->endpoint_address);
    }
    if (!found) kprintf("XHCI: no devices attached\n");
}

/* Start the controller and prove the rings work, which a No-Op command does
   exactly: it asks the controller for nothing except an answer. */
static int start_controller(void) {
    xhci_write32(controller.operational + XHCI_USBCMD,
                 xhci_read32(controller.operational + XHCI_USBCMD) | USBCMD_RUN);
    if (wait_for(controller.operational + XHCI_USBSTS, USBSTS_HALTED, 0, RUN_TIMEOUT_NS) != 0) {
        kprintf("XHCI: controller will not run\n");
        return -1;
    }

    submit_command(TRB_TYPE_NO_OP_COMMAND, 0);
    uint32_t completion = 0;
    if (wait_for_command(&completion) != 0) {
        kprintf("XHCI: no answer to the no-op command\n");
        return -1;
    }
    if (completion != TRB_COMPLETION_SUCCESS) {
        kprintf("XHCI: no-op command failed with code %u\n", (unsigned)completion);
        return -1;
    }
    return 0;
}

int xhci_init(void) {
    struct pci_device device;
    if (pci_find_class(PCI_CLASS_SERIAL_BUS, PCI_SUBCLASS_USB, &device) != 0) return -1;
    if (device.prog_if != PCI_PROG_IF_XHCI) {
        kprintf("XHCI: the USB controller is not xHCI (prog-if %x)\n", device.prog_if);
        return -1;
    }

    /* BAR0 is memory-mapped and, on every real xHCI, 64-bit -- which means it
       is a pair of BARs and the high half lives in BAR1. */
    if (device.bar[0] & PCI_BAR_IO) return -1;
    uint64_t physical = device.bar[0] & PCI_BAR_ADDRESS_MASK;
    if ((device.bar[0] & PCI_BAR_TYPE_MASK) == PCI_BAR_TYPE_64BIT)
        physical |= (uint64_t)device.bar[1] << 32;
    if (!physical) return -1;

    controller.base = map_registers(physical);
    if (!controller.base) {
        kprintf("XHCI: could not map registers at %x\n", (unsigned)physical);
        return -1;
    }

    /* Bus mastering off means every ring this driver builds is invisible to
       the controller, and nothing fails loudly -- it just never answers. */
    pci_enable_bus_mastering(&device);

    uint32_t length_and_version = mmio_read32(controller.base + XHCI_CAPLENGTH);
    uint8_t capability_length = (uint8_t)length_and_version;
    uint32_t structural = mmio_read32(controller.base + XHCI_HCSPARAMS1);
    uint32_t capabilities = mmio_read32(controller.base + XHCI_HCCPARAMS1);

    controller.operational = controller.base + capability_length;
    controller.runtime = controller.base + (mmio_read32(controller.base + XHCI_RTSOFF) & ~0x1FU);
    controller.doorbell = controller.base + (mmio_read32(controller.base + XHCI_DBOFF) & ~0x3U);
    controller.max_slots = (structural >> HCSPARAMS1_MAX_SLOTS_SHIFT) & HCSPARAMS1_MAX_SLOTS_MASK;
    controller.max_ports = (structural >> HCSPARAMS1_MAX_PORTS_SHIFT) & HCSPARAMS1_MAX_PORTS_MASK;
    controller.context_bytes = (capabilities & HCCPARAMS1_CONTEXT_64) ? 64U : 32U;
    controller.version = (uint16_t)(length_and_version >> XHCI_VERSION_SHIFT);

    if (!controller.max_slots || !controller.max_ports) {
        kprintf("XHCI: controller reports no slots or no ports\n");
        return -1;
    }

    if (reset_controller() != 0) return -1;

    interrupter = controller.runtime + XHCI_INTERRUPTER0;
    if (build_device_contexts() != 0 || build_command_ring() != 0 ||
        build_event_ring() != 0) {
        kprintf("XHCI: could not build the controller's rings\n");
        return -1;
    }
    if (start_controller() != 0) return -1;

    controller.present = 1;
    kprintf("XHCI: %x.%x, %u slots, %u ports, %u-byte contexts, rings answering\n",
            (unsigned)(controller.version >> 8), (unsigned)(controller.version & 0xFF),
            (unsigned)controller.max_slots, (unsigned)controller.max_ports,
            (unsigned)controller.context_bytes);
    enumerate_ports();
    return 0;
}

/* --- what the mass-storage driver above needs ---------------------------- */

static struct xhci_device *storage_device(int index) {
    int seen = 0;
    for (unsigned at = 0; at < MAX_DEVICES; at++) {
        if (!devices[at].used || !devices[at].is_storage) continue;
        if (seen == index) return &devices[at];
        seen++;
    }
    return NULL;
}

int xhci_storage_count(void) {
    int count = 0;
    for (unsigned at = 0; at < MAX_DEVICES; at++)
        if (devices[at].used && devices[at].is_storage) count++;
    return count;
}

/*
 * One bulk transfer, submitted and waited for. Everything the transport above
 * does -- command, data, status -- is one of these, and each is a single TRB
 * because the buffers handed down are already physically contiguous.
 */
int xhci_bulk_transfer(int index, int in, uint64_t physical, uint32_t length) {
    struct xhci_device *device = storage_device(index);
    if (!device) return -1;
    struct producer_ring *ring = in ? &device->bulk_in_ring : &device->bulk_out_ring;
    uint32_t dci = in ? device->bulk_in_dci : device->bulk_out_dci;

    enqueue(ring, physical, length, TRB_TYPE_NORMAL, TRB_INTERRUPT_ON_COMPLETION);
    ring_doorbell(device->slot, dci);
    return wait_for_transfer();
}
