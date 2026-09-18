#include <stdint.h>
#include <stddef.h>

#include "../../include/cpu.h"
#include "../../include/hid.h"
#include "../../include/irq.h"
#include "../../include/pci.h"
#include "../../include/pmm.h"
#include "../../include/vmm.h"
#include "../../include/time.h"
#include "../../include/kstring.h"
#include "../../include/input.h"
#include "../../include/tunix/input_event.h"
#include "../../include/usb.h"
#include "../../include/xhci.h"

extern void kprintf(const char *fmt, ...);

#define PCI_CLASS_SERIAL_BUS 0x0CU
#define PCI_SUBCLASS_USB 0x03U
#define PCI_PROG_IF_XHCI 0x30U

#define XHCI_CAPLENGTH 0x00U
#define XHCI_VERSION_SHIFT 16U
#define XHCI_HCSPARAMS1 0x04U
#define XHCI_HCSPARAMS2 0x08U
#define XHCI_HCCPARAMS1 0x10U
#define XHCI_DBOFF 0x14U
#define XHCI_RTSOFF 0x18U
#define XHCI_REGISTER_BYTES 0x10000ULL

#define HCCPARAMS1_AC64 (1U << 0)
#define HCCPARAMS1_CONTEXT_64 (1U << 2)
#define HCCPARAMS1_PPC (1U << 3)
#define HCCPARAMS1_XECP_SHIFT 16U

#define XHCI_USBCMD 0x00U
#define XHCI_USBSTS 0x04U
#define XHCI_CRCR 0x18U
#define XHCI_DCBAAP 0x30U
#define XHCI_CONFIG 0x38U
#define XHCI_PORTSC_BASE 0x400U
#define XHCI_PORT_STRIDE 0x10U

#define XHCI_INTERRUPTER0 0x20U
#define XHCI_IMAN 0x00U
#define XHCI_IMOD 0x04U
#define XHCI_ERSTSZ 0x08U
#define XHCI_ERSTBA 0x10U
#define XHCI_ERDP 0x18U

#define USBCMD_RUN (1U << 0)
#define USBCMD_RESET (1U << 1)
#define USBCMD_INTERRUPTS (1U << 2)
#define USBSTS_HALTED (1U << 0)
#define USBSTS_HOST_ERROR (1U << 2)
#define USBSTS_EVENT_INTERRUPT (1U << 3)
#define USBSTS_NOT_READY (1U << 11)
#define IMAN_PENDING (1U << 0)
#define IMAN_ENABLE (1U << 1)
#define CRCR_RING_CYCLE_STATE (1ULL << 0)
#define ERDP_EVENT_HANDLER_BUSY (1ULL << 3)

#define XECP_LEGACY 1U
#define LEGACY_BIOS_OWNED (1U << 16)
#define LEGACY_OS_OWNED (1U << 24)
#define LEGACY_SMI_ENABLES ((1U << 0) | (1U << 4) | (0x7U << 13))
#define LEGACY_SMI_EVENTS (0x7U << 29)

#define PORTSC_CONNECTED (1U << 0)
#define PORTSC_ENABLED (1U << 1)
#define PORTSC_RESET (1U << 4)
#define PORTSC_POWER (1U << 9)
#define PORTSC_SPEED_SHIFT 10U
#define PORTSC_SPEED_MASK 0xFU
#define PORTSC_CONNECT_CHANGE (1U << 17)
#define PORTSC_RESET_CHANGE (1U << 21)
#define PORTSC_CHANGES 0x00FE0000U
#define PORTSC_NEUTRAL 0x4E00FDE9U

#define TRB_BYTES 16U
#define TRB_TYPE_SHIFT 10U
#define TRB_TYPE_MASK 0x3FU
#define TRB_CYCLE (1U << 0)
#define TRB_TOGGLE_CYCLE (1U << 1)
#define TRB_INTERRUPT_ON_SHORT (1U << 2)
#define TRB_INTERRUPT_ON_COMPLETION (1U << 5)
#define TRB_IMMEDIATE_DATA (1U << 6)
#define TRB_DIRECTION_IN (1U << 16)
#define TRB_TRANSFER_TYPE_SHIFT 16U
#define TRB_TRANSFER_TYPE_NO_DATA 0U
#define TRB_TRANSFER_TYPE_OUT 2U
#define TRB_TRANSFER_TYPE_IN 3U

#define TRB_TYPE_NORMAL 1U
#define TRB_TYPE_SETUP_STAGE 2U
#define TRB_TYPE_DATA_STAGE 3U
#define TRB_TYPE_STATUS_STAGE 4U
#define TRB_TYPE_LINK 6U
#define TRB_TYPE_ENABLE_SLOT 9U
#define TRB_TYPE_DISABLE_SLOT 10U
#define TRB_TYPE_ADDRESS_DEVICE 11U
#define TRB_TYPE_CONFIGURE_ENDPOINT 12U
#define TRB_TYPE_EVALUATE_CONTEXT 13U
#define TRB_TYPE_RESET_ENDPOINT 14U
#define TRB_TYPE_STOP_ENDPOINT 15U
#define TRB_TYPE_SET_DEQUEUE 16U
#define TRB_TYPE_NO_OP_COMMAND 23U
#define TRB_TYPE_TRANSFER_EVENT 32U
#define TRB_TYPE_COMMAND_COMPLETION 33U
#define TRB_TYPE_PORT_STATUS_CHANGE 34U
#define TRB_TYPE_HOST_CONTROLLER_EVENT 37U

#define TRB_COMPLETION_SHIFT 24U
#define TRB_COMPLETION_MASK 0xFFU
#define CODE_SUCCESS 1U
#define CODE_BABBLE 3U
#define CODE_TRANSACTION 4U
#define CODE_STALL 6U
#define CODE_SHORT_PACKET 13U
#define CODE_STOPPED 26U
#define CODE_STOPPED_LENGTH 27U
#define CODE_TIMEOUT 0x100U
#define TRANSFER_RESIDUAL_MASK 0x00FFFFFFU
#define EVENT_SLOT_SHIFT 24U
#define EVENT_ENDPOINT_SHIFT 16U
#define EVENT_ENDPOINT_MASK 0x1FU
#define EVENT_PORT_SHIFT 24U

#define RING_BYTES 4096U
#define RING_TRB_COUNT (RING_BYTES / TRB_BYTES)

#define INPUT_CONTROL_INDEX 0U
#define SLOT_CONTEXT_INDEX 1U
#define ADD_SLOT (1U << 0)
#define ADD_EP0 (1U << 1)
#define SLOT_ROUTE_MASK 0xFFFFFU
#define SLOT_SPEED_SHIFT 20U
#define SLOT_HUB (1U << 26)
#define SLOT_ENTRIES_SHIFT 27U
#define SLOT_ROOT_PORT_SHIFT 16U
#define SLOT_PORT_COUNT_SHIFT 24U
#define SLOT_TT_PORT_SHIFT 8U
#define SLOT_TT_THINK_SHIFT 16U

#define EP_STATE_MASK 0x7U
#define EP_STATE_HALTED 2U
#define EP_STATE_STOPPED 3U
#define EP_INTERVAL_SHIFT 16U
#define EP_TYPE_SHIFT 3U
#define EP_ERROR_COUNT_SHIFT 1U
#define EP_ERROR_COUNT 3U
#define EP_MAX_PACKET_SHIFT 16U
#define EP_MAX_ESIT_SHIFT 16U
#define EP_DEQUEUE_CYCLE 1U
#define EP_TYPE_CONTROL 4U
#define EP_TYPE_BULK_OUT 2U
#define EP_TYPE_BULK_IN 6U
#define EP_TYPE_INTERRUPT_IN 7U
#define BULK_AVERAGE_TRB 3072U

#define USB_SPEED_FULL 1U
#define USB_SPEED_LOW 2U
#define USB_SPEED_HIGH 3U
#define USB_SPEED_SUPER 4U

#define USB_DIRECTION_IN 0x80U
#define REQUEST_TYPE_CLASS_INTERFACE 0x21U
#define REQUEST_TYPE_CLASS_DEVICE_IN 0xA0U
#define REQUEST_TYPE_CLASS_OTHER_OUT 0x23U
#define REQUEST_TYPE_CLASS_OTHER_IN 0xA3U
#define REQUEST_TYPE_STANDARD_ENDPOINT 0x02U
#define REQUEST_GET_STATUS 0U
#define REQUEST_CLEAR_FEATURE 1U
#define REQUEST_SET_FEATURE 3U
#define REQUEST_GET_DESCRIPTOR 6U
#define REQUEST_SET_CONFIGURATION 9U
#define REQUEST_HID_SET_IDLE 0x0AU
#define REQUEST_HID_SET_PROTOCOL 0x0BU
#define REQUEST_STORAGE_RESET 0xFFU
#define FEATURE_ENDPOINT_HALT 0U

#define DESCRIPTOR_DEVICE 1U
#define DESCRIPTOR_CONFIGURATION 2U
#define DESCRIPTOR_INTERFACE 4U
#define DESCRIPTOR_ENDPOINT 5U
#define DESCRIPTOR_HUB 0x29U
#define DESCRIPTOR_HID 0x21U
#define DESCRIPTOR_HID_REPORT 0x22U
#define REQUEST_TYPE_STANDARD_INTERFACE_IN 0x81U
#define HID_PROTOCOL_REPORT 1U

#define CLASS_HID 3U
#define CLASS_STORAGE 8U
#define CLASS_HUB 9U
#define HID_SUBCLASS_BOOT 1U
#define HID_KEYBOARD 1U
#define HID_MOUSE 2U
#define STORAGE_SUBCLASS_SCSI 6U
#define STORAGE_BULK_ONLY 0x50U

#define ENDPOINT_DIRECTION_IN 0x80U
#define ENDPOINT_NUMBER_MASK 0x0FU
#define ENDPOINT_TYPE_MASK 0x03U
#define ENDPOINT_TYPE_BULK 2U
#define ENDPOINT_TYPE_INTERRUPT 3U

#define HUB_FEATURE_PORT_RESET 4U
#define HUB_FEATURE_PORT_POWER 8U
#define HUB_FEATURE_C_CONNECTION 16U
#define HUB_FEATURE_C_ENABLE 17U
#define HUB_FEATURE_C_SUSPEND 18U
#define HUB_FEATURE_C_OVER_CURRENT 19U
#define HUB_FEATURE_C_RESET 20U
#define HUB_STATUS_CONNECTION 0x0001U
#define HUB_STATUS_ENABLE 0x0002U
#define HUB_STATUS_LOW_SPEED 0x0200U
#define HUB_STATUS_HIGH_SPEED 0x0400U
#define HUB_CHANGE_CONNECTION 0x0001U
#define HUB_CHANGE_ENABLE 0x0002U
#define HUB_CHANGE_SUSPEND 0x0004U
#define HUB_CHANGE_OVER_CURRENT 0x0008U
#define HUB_CHANGE_RESET 0x0010U
#define HUB_MAX_PORTS 15U
#define HUB_MAX_DEPTH 5U

#define KEYBOARD_REPORT_BYTES 8U
#define KEYBOARD_REPORT_KEYS 6U
#define KEYBOARD_REPORT_KEYS_OFFSET 2U
#define KEYBOARD_MODIFIER_COUNT 8U
#define HID_USAGE_FIRST_KEY 4U
#define MOUSE_BUTTON_MASK 0x07U

#define MAX_CONTROLLERS 4U
#define MAX_DEVICES 32U
#define MAX_HID 4U
#define MAX_STORAGE 16U
#define MAX_PORTS 256U
#define RECOVERY_LIMIT 8U

#define NS_PER_MS 1000000ULL
#define RESET_TIMEOUT_NS (1000ULL * NS_PER_MS)
#define HALT_TIMEOUT_NS (100ULL * NS_PER_MS)
#define HANDOFF_TIMEOUT_NS (1000ULL * NS_PER_MS)
#define COMMAND_TIMEOUT_NS (1000ULL * NS_PER_MS)
#define CONTROL_TIMEOUT_NS (1000ULL * NS_PER_MS)
#define BULK_TIMEOUT_NS (5000ULL * NS_PER_MS)
#define PORT_RESET_TIMEOUT_NS (500ULL * NS_PER_MS)
#define PORT_POWER_NS (20ULL * NS_PER_MS)
#define DEBOUNCE_NS (100ULL * NS_PER_MS)
#define RESET_RECOVERY_NS (10ULL * NS_PER_MS)
#define FIRST_SCAN_NS (200ULL * NS_PER_MS)
#define DMA_LIMIT_32 0x100000000ULL
#define DMA_ATTEMPTS 64U

struct trb {
    uint32_t parameter_low;
    uint32_t parameter_high;
    uint32_t status;
    uint32_t control;
};

struct ring {
    struct trb *entries;
    uint64_t physical;
    uint32_t index;
    uint32_t cycle;
};

struct endpoint_result {
    int done;
    uint32_t code;
    uint32_t residual;
};

struct hid_function {
    uint8_t protocol;
    uint8_t subclass;
    uint8_t interface_protocol;
    uint16_t descriptor_length;
    int report_mode;
    struct hid_mouse_layout layout;
    uint8_t interface;
    uint8_t address;
    uint8_t interval;
    uint16_t packet;
    uint32_t dci;
    struct ring ring;
    uint8_t *report;
    uint64_t report_physical;
    uint8_t previous[KEYBOARD_REPORT_BYTES];
    int needs_recovery;
    int stalled;
    unsigned recoveries;
};

struct xhci_host;

struct usb_device {
    int used;
    struct xhci_host *host;
    uint32_t slot;
    uint32_t speed;
    uint32_t root_port;
    uint32_t route;
    uint32_t depth;
    uint32_t tt_slot;
    uint32_t tt_port;
    struct usb_device *parent;
    uint32_t parent_port;
    uint32_t ep0_packet;
    uint32_t *input_context;
    uint64_t input_physical;
    uint32_t *output_context;
    uint64_t output_physical;
    uint8_t *buffer;
    uint64_t buffer_physical;
    struct ring ep0;
    struct endpoint_result results[32];
    uint8_t configuration;
    uint8_t device_class;

    struct hid_function hid[MAX_HID];
    unsigned hid_count;
    uint8_t interfaces[8][3];
    unsigned interface_count;

    int storage;
    uint8_t storage_interface;
    uint8_t bulk_in_address;
    uint8_t bulk_out_address;
    uint16_t bulk_in_packet;
    uint16_t bulk_out_packet;
    uint32_t bulk_in_dci;
    uint32_t bulk_out_dci;
    struct ring bulk_in;
    struct ring bulk_out;

    int hub;
    uint32_t hub_ports;
    uint32_t hub_think;
    uint32_t hub_power_ms;
    uint8_t hub_address;
    uint16_t hub_packet;
    uint8_t hub_interval;
    uint32_t hub_dci;
    struct ring hub_ring;
    uint8_t *hub_bitmap;
    uint64_t hub_bitmap_physical;
    uint32_t hub_changed;
    uint64_t hub_seen[HUB_MAX_PORTS + 1U];
    int hub_needs_recovery;
    unsigned hub_recoveries;
    int started;
    int disconnected;
    char name[24];
};

struct xhci_host {
    int present;
    unsigned index;
    struct pci_device pci;
    uint64_t base;
    uint64_t operational;
    uint64_t runtime;
    uint64_t doorbell;
    uint64_t interrupter;
    uint16_t version;
    uint32_t max_slots;
    uint32_t max_ports;
    uint32_t context_bytes;
    int ac64;
    int port_power;
    struct ring commands;
    struct ring events;
    uint64_t *dcbaa;
    int command_done;
    uint32_t command_code;
    uint32_t command_slot;
    struct usb_device devices[MAX_DEVICES];
    struct usb_device *by_slot[256];
    uint64_t root_changed[4];
    uint64_t root_seen[MAX_PORTS];
    int pumping;
    int failed;
    unsigned vector;
};

struct storage_entry {
    struct xhci_host *host;
    struct usb_device *device;
};

static struct xhci_host hosts[MAX_CONTROLLERS];
static unsigned host_count;
static struct storage_entry storage_table[MAX_STORAGE];
static int storage_entries;
static int servicing;
static int booted;

static const struct usb_host xhci_usb_host;

static inline uint32_t read32(uint64_t address) {
    return *(volatile uint32_t *)address;
}

static inline void write32(uint64_t address, uint32_t value) {
    *(volatile uint32_t *)address = value;
}

static void write64(uint64_t address, uint64_t value) {
    write32(address, (uint32_t)value);
    write32(address + 4U, (uint32_t)(value >> 32));
}

static uint64_t now(void) { return time_uptime_ns(); }

static void pause_ns(uint64_t nanoseconds) {
    uint64_t until = now() + nanoseconds;
    while (now() < until) cpu_relax();
}

static int wait_bits(uint64_t address, uint32_t mask, uint32_t wanted, uint64_t timeout) {
    uint64_t deadline = now() + timeout;
    for (;;) {
        if ((read32(address) & mask) == wanted) return 0;
        if (now() >= deadline) return -1;
        cpu_relax();
    }
}

static void *page_for(struct xhci_host *host, uint64_t *physical_out) {
    void *rejected[DMA_ATTEMPTS];
    unsigned rejects = 0;
    void *taken = NULL;
    for (unsigned attempt = 0; attempt < DMA_ATTEMPTS; attempt++) {
        void *page = pmm_alloc_page();
        if (!page) break;
        if (host->ac64 || (uint64_t)page < DMA_LIMIT_32) {
            taken = page;
            break;
        }
        rejected[rejects++] = page;
    }
    while (rejects) pmm_free_page(rejected[--rejects]);
    if (!taken) return NULL;
    void *mapped = vmm_phys_to_virt((uint64_t)taken);
    if (!mapped) {
        pmm_free_page(taken);
        return NULL;
    }
    memset(mapped, 0, RING_BYTES);
    *physical_out = (uint64_t)taken;
    return mapped;
}

static void page_release(uint64_t physical) {
    if (physical) pmm_free_page((void *)physical);
}

static int ring_create(struct xhci_host *host, struct ring *ring) {
    ring->entries = page_for(host, &ring->physical);
    if (!ring->entries) return -1;
    ring->index = 0;
    ring->cycle = 1;
    struct trb *link = &ring->entries[RING_TRB_COUNT - 1];
    link->parameter_low = (uint32_t)ring->physical;
    link->parameter_high = (uint32_t)(ring->physical >> 32);
    link->control = (TRB_TYPE_LINK << TRB_TYPE_SHIFT) | TRB_TOGGLE_CYCLE;
    return 0;
}

static void ring_release(struct ring *ring) {
    page_release(ring->physical);
    memset(ring, 0, sizeof(*ring));
}

static uint64_t ring_position(const struct ring *ring) {
    return ring->physical + (uint64_t)ring->index * TRB_BYTES;
}

static void enqueue(struct ring *ring, uint64_t parameter, uint32_t status,
                    uint32_t type, uint32_t control) {
    struct trb *entry = &ring->entries[ring->index];
    entry->parameter_low = (uint32_t)parameter;
    entry->parameter_high = (uint32_t)(parameter >> 32);
    entry->status = status;
    __asm__ volatile("" ::: "memory");
    entry->control = (type << TRB_TYPE_SHIFT) | control | ring->cycle;
    ring->index++;
    if (ring->index == RING_TRB_COUNT - 1) {
        struct trb *link = &ring->entries[RING_TRB_COUNT - 1];
        link->control = (link->control & ~TRB_CYCLE) | ring->cycle;
        ring->index = 0;
        ring->cycle ^= 1U;
    }
}

static void ring_doorbell(struct xhci_host *host, uint32_t slot, uint32_t target) {
    __asm__ volatile("" ::: "memory");
    write32(host->doorbell + (uint64_t)slot * 4U, target);
}

static uint32_t *context_at(struct xhci_host *host, uint32_t *base, uint32_t index) {
    return base + (index * host->context_bytes) / sizeof(uint32_t);
}

static uint64_t port_register(struct xhci_host *host, uint32_t port) {
    return host->operational + XHCI_PORTSC_BASE + (uint64_t)(port - 1U) * XHCI_PORT_STRIDE;
}

static void keyboard_report(struct hid_function *function, uint32_t length);
static void check_health(struct xhci_host *host);
static void mouse_report(struct hid_function *function, uint32_t length);

static struct hid_function *hid_for(struct usb_device *device, uint32_t dci) {
    for (unsigned index = 0; index < device->hid_count; index++)
        if (device->hid[index].dci == dci) return &device->hid[index];
    return NULL;
}

static void hid_arm(struct usb_device *device, struct hid_function *function) {
    enqueue(&function->ring, function->report_physical, function->packet, TRB_TYPE_NORMAL,
            TRB_INTERRUPT_ON_COMPLETION | TRB_INTERRUPT_ON_SHORT);
    ring_doorbell(device->host, device->slot, function->dci);
}

static void hub_arm(struct usb_device *device) {
    enqueue(&device->hub_ring, device->hub_bitmap_physical, device->hub_packet, TRB_TYPE_NORMAL,
            TRB_INTERRUPT_ON_COMPLETION | TRB_INTERRUPT_ON_SHORT);
    ring_doorbell(device->host, device->slot, device->hub_dci);
}

static int completed(uint32_t code) {
    return code == CODE_SUCCESS || code == CODE_SHORT_PACKET;
}

static void mark_subtree(struct xhci_host *host, struct usb_device *device) {
    device->disconnected = 1;
    for (unsigned index = 0; index < MAX_DEVICES; index++) {
        struct usb_device *child = &host->devices[index];
        if (child->used && child->parent == device && !child->disconnected) mark_subtree(host, child);
    }
}

static void mark_hub_ports(struct xhci_host *host, struct usb_device *hub, uint32_t changed) {
    for (unsigned index = 0; index < MAX_DEVICES; index++) {
        struct usb_device *child = &host->devices[index];
        if (child->used && child->started && child->parent == hub &&
            child->parent_port < 32U && (changed & (1U << child->parent_port)))
            mark_subtree(host, child);
    }
}

static void transfer_event(struct xhci_host *host, const struct trb *event) {
    uint32_t slot = (event->control >> EVENT_SLOT_SHIFT) & 0xFFU;
    uint32_t dci = (event->control >> EVENT_ENDPOINT_SHIFT) & EVENT_ENDPOINT_MASK;
    uint32_t code = (event->status >> TRB_COMPLETION_SHIFT) & TRB_COMPLETION_MASK;
    uint32_t residual = event->status & TRANSFER_RESIDUAL_MASK;
    struct usb_device *device = host->by_slot[slot];
    if (!device || !device->used) return;
    if (code == CODE_STOPPED || code == CODE_STOPPED_LENGTH) {
        device->results[dci].done = 1;
        device->results[dci].code = code;
        device->results[dci].residual = residual;
        return;
    }

    struct hid_function *function = hid_for(device, dci);
    if (function) {
        if (!completed(code)) {
            function->needs_recovery = 1;
            function->stalled = code == CODE_STALL;
            return;
        }
        uint32_t length = residual < function->packet ? function->packet - residual : 0;
        if (function->protocol == HID_KEYBOARD) keyboard_report(function, length);
        else mouse_report(function, length);
        hid_arm(device, function);
        return;
    }
    if (device->hub && dci == device->hub_dci) {
        if (!completed(code)) {
            device->hub_needs_recovery = 1;
            return;
        }
        uint32_t length = residual < device->hub_packet ? device->hub_packet - residual : 0;
        uint32_t changed = 0;
        for (uint32_t byte = 0; byte < length && byte < 4U; byte++)
            changed |= (uint32_t)device->hub_bitmap[byte] << (byte * 8U);
        device->hub_changed |= changed;
        mark_hub_ports(host, device, changed);
        hub_arm(device);
        return;
    }
    device->results[dci].done = 1;
    device->results[dci].code = code;
    device->results[dci].residual = residual;
}

static void mark_disconnected(struct xhci_host *host, uint32_t port) {
    for (unsigned index = 0; index < MAX_DEVICES; index++) {
        struct usb_device *device = &host->devices[index];
        if (device->used && device->started && device->root_port == port) device->disconnected = 1;
    }
}

static void pump(struct xhci_host *host) {
    if (host->pumping || !host->present) return;
    host->pumping = 1;
    unsigned consumed = 0;
    for (;;) {
        struct trb *entry = &host->events.entries[host->events.index];
        if ((entry->control & TRB_CYCLE) != host->events.cycle) break;
        struct trb event = *entry;
        host->events.index++;
        if (host->events.index == RING_TRB_COUNT) {
            host->events.index = 0;
            host->events.cycle ^= 1U;
        }
        consumed++;
        uint32_t type = (event.control >> TRB_TYPE_SHIFT) & TRB_TYPE_MASK;
        if (type == TRB_TYPE_COMMAND_COMPLETION) {
            host->command_done = 1;
            host->command_code = (event.status >> TRB_COMPLETION_SHIFT) & TRB_COMPLETION_MASK;
            host->command_slot = (event.control >> EVENT_SLOT_SHIFT) & 0xFFU;
        } else if (type == TRB_TYPE_TRANSFER_EVENT) {
            transfer_event(host, &event);
        } else if (type == TRB_TYPE_PORT_STATUS_CHANGE) {
            uint32_t port = (event.parameter_low >> EVENT_PORT_SHIFT) & 0xFFU;
            if (port && port <= host->max_ports) {
                host->root_changed[port / 64U] |= 1ULL << (port % 64U);
                uint32_t status = read32(port_register(host, port));
                if (!(status & PORTSC_CONNECTED) || (status & PORTSC_CONNECT_CHANGE))
                    mark_disconnected(host, port);
            }
        } else if (type == TRB_TYPE_HOST_CONTROLLER_EVENT) {
            kprintf("XHCI%u: controller event %u\n", host->index,
                    (unsigned)((event.status >> TRB_COMPLETION_SHIFT) & TRB_COMPLETION_MASK));
        }
    }
    if (consumed)
        write64(host->interrupter + XHCI_ERDP,
                ring_position(&host->events) | ERDP_EVENT_HANDLER_BUSY);
    host->pumping = 0;
}

static int command(struct xhci_host *host, uint32_t type, uint64_t parameter,
                   uint32_t control, uint32_t *slot_out) {
    if (host->failed) return -1;
    host->command_done = 0;
    enqueue(&host->commands, parameter, 0, type, control);
    ring_doorbell(host, 0, 0);
    uint64_t deadline = now() + COMMAND_TIMEOUT_NS;
    while (!host->command_done) {
        pump(host);
        if (host->command_done) break;
        if (now() >= deadline) {
            check_health(host);
            kprintf("XHCI%u: command %u was never answered\n", host->index, (unsigned)type);
            return -1;
        }
        cpu_relax();
    }
    if (slot_out) *slot_out = host->command_slot;
    return host->command_code == CODE_SUCCESS ? 0 : -(int)host->command_code;
}

static uint32_t wait_result(struct usb_device *device, uint32_t dci, uint64_t timeout,
                            uint32_t *residual) {
    uint64_t deadline = now() + timeout;
    for (;;) {
        pump(device->host);
        if (device->results[dci].done) break;
        if (device->disconnected || device->host->failed || now() >= deadline) return CODE_TIMEOUT;
        cpu_relax();
    }
    if (residual) *residual = device->results[dci].residual;
    return device->results[dci].code;
}

static uint32_t endpoint_state(struct usb_device *device, uint32_t dci) {
    return context_at(device->host, device->output_context, dci)[0] & EP_STATE_MASK;
}

static int endpoint_command(struct usb_device *device, uint32_t type, uint32_t dci) {
    return command(device->host, type, 0,
                   (device->slot << EVENT_SLOT_SHIFT) | (dci << EVENT_ENDPOINT_SHIFT), NULL);
}

static int realign_endpoint(struct usb_device *device, uint32_t dci, struct ring *ring) {
    uint32_t state = endpoint_state(device, dci);
    if (state == EP_STATE_HALTED) {
        if (endpoint_command(device, TRB_TYPE_RESET_ENDPOINT, dci) != 0) return -1;
    } else if (state != EP_STATE_STOPPED) {
        device->results[dci].done = 0;
        (void)endpoint_command(device, TRB_TYPE_STOP_ENDPOINT, dci);
        (void)wait_result(device, dci, 20ULL * NS_PER_MS, NULL);
        if (endpoint_state(device, dci) == EP_STATE_HALTED &&
            endpoint_command(device, TRB_TYPE_RESET_ENDPOINT, dci) != 0)
            return -1;
    }
    return command(device->host, TRB_TYPE_SET_DEQUEUE, ring_position(ring) | ring->cycle,
                   (device->slot << EVENT_SLOT_SHIFT) | (dci << EVENT_ENDPOINT_SHIFT), NULL);
}

static int control(struct usb_device *device, uint8_t request_type, uint8_t request,
                   uint16_t value, uint16_t index, uint16_t length) {
    if (!device->used || device->disconnected || device->host->failed) return -1;
    if (length > RING_BYTES) return -1;
    uint64_t setup = (uint64_t)request_type | ((uint64_t)request << 8) |
                     ((uint64_t)value << 16) | ((uint64_t)index << 32) |
                     ((uint64_t)length << 48);
    uint32_t transfer_type = TRB_TRANSFER_TYPE_NO_DATA;
    if (length)
        transfer_type = (request_type & USB_DIRECTION_IN) ? TRB_TRANSFER_TYPE_IN
                                                          : TRB_TRANSFER_TYPE_OUT;

    device->results[1].done = 0;
    enqueue(&device->ep0, setup, 8U, TRB_TYPE_SETUP_STAGE,
            TRB_IMMEDIATE_DATA | (transfer_type << TRB_TRANSFER_TYPE_SHIFT));
    if (length)
        enqueue(&device->ep0, device->buffer_physical, length, TRB_TYPE_DATA_STAGE,
                (request_type & USB_DIRECTION_IN) ? TRB_DIRECTION_IN : 0);
    uint32_t status_direction = (length && (request_type & USB_DIRECTION_IN)) ? 0 : TRB_DIRECTION_IN;
    enqueue(&device->ep0, 0, 0, TRB_TYPE_STATUS_STAGE,
            status_direction | TRB_INTERRUPT_ON_COMPLETION);
    ring_doorbell(device->host, device->slot, 1);

    uint32_t code = wait_result(device, 1, CONTROL_TIMEOUT_NS, NULL);
    if (completed(code)) return 0;
    if (device->disconnected) return -1;
    (void)realign_endpoint(device, 1, &device->ep0);
    return -1;
}

static int bulk(struct usb_device *device, int in, uint64_t physical, uint32_t length) {
    if (!device->used || device->disconnected || device->host->failed) return -1;
    struct ring *ring = in ? &device->bulk_in : &device->bulk_out;
    uint32_t dci = in ? device->bulk_in_dci : device->bulk_out_dci;
    device->results[dci].done = 0;
    enqueue(ring, physical, length, TRB_TYPE_NORMAL, TRB_INTERRUPT_ON_COMPLETION);
    ring_doorbell(device->host, device->slot, dci);
    uint32_t code = wait_result(device, dci, BULK_TIMEOUT_NS, NULL);
    if (completed(code)) return 0;
    if (device->disconnected) return -1;
    (void)realign_endpoint(device, dci, ring);
    if (code == CODE_STALL)
        (void)control(device, REQUEST_TYPE_STANDARD_ENDPOINT, REQUEST_CLEAR_FEATURE,
                      FEATURE_ENDPOINT_HALT,
                      in ? device->bulk_in_address : device->bulk_out_address, 0);
    return -1;
}

static void write_slot_context(struct usb_device *device, uint32_t entries) {
    uint32_t *slot = context_at(device->host, device->input_context, SLOT_CONTEXT_INDEX);
    slot[0] = (device->route & SLOT_ROUTE_MASK) | (device->speed << SLOT_SPEED_SHIFT) |
              (entries << SLOT_ENTRIES_SHIFT) | (device->hub ? SLOT_HUB : 0);
    slot[1] = (device->root_port << SLOT_ROOT_PORT_SHIFT) |
              (device->hub ? device->hub_ports << SLOT_PORT_COUNT_SHIFT : 0);
    slot[2] = device->tt_slot | (device->tt_port << SLOT_TT_PORT_SHIFT) |
              (device->hub && device->speed == USB_SPEED_HIGH
                   ? device->hub_think << SLOT_TT_THINK_SHIFT : 0);
    slot[3] = 0;
}

static void clear_input(struct usb_device *device) {
    memset(device->input_context, 0, RING_BYTES);
}

static uint32_t interval_for(struct usb_device *device, uint8_t interval) {
    uint32_t exponent;
    if (device->speed == USB_SPEED_LOW || device->speed == USB_SPEED_FULL) {
        uint32_t frames = interval ? interval : 1U;
        exponent = 0;
        while ((1U << (exponent + 1U)) <= frames * 8U) exponent++;
        if (exponent < 3U) exponent = 3U;
        if (exponent > 10U) exponent = 10U;
    } else {
        exponent = interval ? interval - 1U : 0U;
        if (exponent > 15U) exponent = 15U;
    }
    return exponent;
}

static void write_interrupt_endpoint(struct usb_device *device, uint32_t dci, struct ring *ring,
                                     uint16_t packet, uint8_t interval) {
    uint32_t *endpoint = context_at(device->host, device->input_context, dci + 1U);
    endpoint[0] = interval_for(device, interval) << EP_INTERVAL_SHIFT;
    endpoint[1] = (EP_TYPE_INTERRUPT_IN << EP_TYPE_SHIFT) |
                  (EP_ERROR_COUNT << EP_ERROR_COUNT_SHIFT) |
                  ((uint32_t)packet << EP_MAX_PACKET_SHIFT);
    endpoint[2] = (uint32_t)(ring->physical | EP_DEQUEUE_CYCLE);
    endpoint[3] = (uint32_t)(ring->physical >> 32);
    endpoint[4] = (uint32_t)packet | ((uint32_t)packet << EP_MAX_ESIT_SHIFT);
}

static void write_bulk_endpoint(struct usb_device *device, uint32_t dci, struct ring *ring,
                                uint32_t type, uint16_t packet) {
    uint32_t *endpoint = context_at(device->host, device->input_context, dci + 1U);
    endpoint[1] = (type << EP_TYPE_SHIFT) | (EP_ERROR_COUNT << EP_ERROR_COUNT_SHIFT) |
                  ((uint32_t)packet << EP_MAX_PACKET_SHIFT);
    endpoint[2] = (uint32_t)(ring->physical | EP_DEQUEUE_CYCLE);
    endpoint[3] = (uint32_t)(ring->physical >> 32);
    endpoint[4] = BULK_AVERAGE_TRB;
}

static int configure(struct usb_device *device, uint32_t added, uint32_t entries) {
    uint32_t *control_context = context_at(device->host, device->input_context, INPUT_CONTROL_INDEX);
    control_context[1] = ADD_SLOT | added;
    write_slot_context(device, entries);
    return command(device->host, TRB_TYPE_CONFIGURE_ENDPOINT, device->input_physical,
                   device->slot << EVENT_SLOT_SHIFT, NULL);
}

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

static void keyboard_apply(struct hid_function *function, const uint8_t *now_report) {
    const uint8_t *before = function->previous;
    for (unsigned bit = 0; bit < KEYBOARD_MODIFIER_COUNT; bit++) {
        uint8_t mask = (uint8_t)(1U << bit);
        if ((now_report[0] & mask) == (before[0] & mask)) continue;
        input_external_key(keycode_for_modifier(bit), (now_report[0] & mask) ? 0 : 1);
    }
    for (unsigned i = 0; i < KEYBOARD_REPORT_KEYS; i++) {
        uint8_t usage = before[KEYBOARD_REPORT_KEYS_OFFSET + i];
        if (usage < HID_USAGE_FIRST_KEY) continue;
        int still_held = 0;
        for (unsigned j = 0; j < KEYBOARD_REPORT_KEYS; j++)
            if (now_report[KEYBOARD_REPORT_KEYS_OFFSET + j] == usage) still_held = 1;
        if (!still_held) input_external_key(keycode_for_usage(usage), 1);
    }
    for (unsigned i = 0; i < KEYBOARD_REPORT_KEYS; i++) {
        uint8_t usage = now_report[KEYBOARD_REPORT_KEYS_OFFSET + i];
        if (usage < HID_USAGE_FIRST_KEY) continue;
        int was_held = 0;
        for (unsigned j = 0; j < KEYBOARD_REPORT_KEYS; j++)
            if (before[KEYBOARD_REPORT_KEYS_OFFSET + j] == usage) was_held = 1;
        if (!was_held) input_external_key(keycode_for_usage(usage), 0);
    }
    memcpy(function->previous, now_report, KEYBOARD_REPORT_BYTES);
}

static void keyboard_report(struct hid_function *function, uint32_t length) {
    if (length < 3U) return;
    uint8_t report[KEYBOARD_REPORT_BYTES];
    memset(report, 0, sizeof(report));
    memcpy(report, function->report, length < KEYBOARD_REPORT_BYTES ? length : KEYBOARD_REPORT_BYTES);
    if (report[KEYBOARD_REPORT_KEYS_OFFSET] == 1U) return;
    keyboard_apply(function, report);
}

static void mouse_report(struct hid_function *function, uint32_t length) {
    if (function->report_mode) {
        int dx, dy, wheel;
        uint32_t buttons;
        if (hid_decode_mouse(&function->layout, function->report, length, &dx, &dy, &wheel, &buttons) != 0)
            return;
        input_external_mouse(dx, dy, wheel, (uint8_t)(buttons & MOUSE_BUTTON_MASK));
        return;
    }
    if (length < 3U) return;
    const uint8_t *report = function->report;
    int wheel = length >= 4U ? (int)(int8_t)report[3] : 0;
    input_external_mouse((int)(int8_t)report[1], (int)(int8_t)report[2], wheel,
                         report[0] & MOUSE_BUTTON_MASK);
}

static void name_device(struct usb_device *device) {
    char text[24];
    unsigned at = 0;
    text[at++] = (char)('0' + device->host->index);
    text[at++] = '-';
    uint32_t port = device->root_port;
    if (port >= 100U) text[at++] = (char)('0' + port / 100U);
    if (port >= 10U) text[at++] = (char)('0' + (port / 10U) % 10U);
    text[at++] = (char)('0' + port % 10U);
    for (uint32_t tier = 0; tier < device->depth && at + 4U < sizeof(text); tier++) {
        uint32_t hop = (device->route >> (4U * tier)) & 0xFU;
        text[at++] = '.';
        if (hop >= 10U) text[at++] = '1';
        text[at++] = (char)('0' + hop % 10U);
    }
    text[at] = '\0';
    memcpy(device->name, text, at + 1U);
}

static struct usb_device *allocate_device(struct xhci_host *host) {
    for (unsigned index = 0; index < MAX_DEVICES; index++) {
        struct usb_device *device = &host->devices[index];
        if (device->used) continue;
        memset(device, 0, sizeof(*device));
        device->host = host;
        device->storage = -1;
        return device;
    }
    return NULL;
}

static void release_device_memory(struct usb_device *device) {
    for (unsigned index = 0; index < device->hid_count; index++) {
        ring_release(&device->hid[index].ring);
        page_release(device->hid[index].report_physical);
    }
    ring_release(&device->bulk_in);
    ring_release(&device->bulk_out);
    ring_release(&device->hub_ring);
    page_release(device->hub_bitmap_physical);
    ring_release(&device->ep0);
    page_release(device->buffer_physical);
    page_release(device->input_physical);
    page_release(device->output_physical);
}

static void remove_device(struct usb_device *device) {
    if (!device || !device->used) return;
    struct xhci_host *host = device->host;
    for (unsigned index = 0; index < MAX_DEVICES; index++) {
        struct usb_device *child = &host->devices[index];
        if (child->used && child->parent == device) remove_device(child);
    }
    for (unsigned index = 0; index < device->hid_count; index++) {
        struct hid_function *function = &device->hid[index];
        if (function->protocol != HID_KEYBOARD) continue;
        uint8_t released[KEYBOARD_REPORT_BYTES];
        memset(released, 0, sizeof(released));
        keyboard_apply(function, released);
    }
    if (device->storage >= 0) storage_table[device->storage].device = NULL;
    if (device->started) kprintf("XHCI: %s slot %u removed\n", device->name, (unsigned)device->slot);
    if (device->slot) {
        host->by_slot[device->slot] = NULL;
        (void)command(host, TRB_TYPE_DISABLE_SLOT, 0, device->slot << EVENT_SLOT_SHIFT, NULL);
        host->dcbaa[device->slot] = 0;
    }
    device->used = 0;
    release_device_memory(device);
}

static int setup_ep0_packet(struct usb_device *device) {
    if (control(device, USB_DIRECTION_IN, REQUEST_GET_DESCRIPTOR,
                DESCRIPTOR_DEVICE << 8, 0, 8) != 0) return -1;
    uint32_t reported = device->buffer[7];
    if (device->speed >= USB_SPEED_SUPER) reported = reported < 16U ? 1U << reported : 512U;
    if (!reported || reported == device->ep0_packet) return 0;
    device->ep0_packet = reported;
    clear_input(device);
    uint32_t *control_context = context_at(device->host, device->input_context, INPUT_CONTROL_INDEX);
    control_context[1] = ADD_EP0;
    uint32_t *endpoint = context_at(device->host, device->input_context, 2);
    endpoint[1] = (EP_TYPE_CONTROL << EP_TYPE_SHIFT) | (EP_ERROR_COUNT << EP_ERROR_COUNT_SHIFT) |
                  (reported << EP_MAX_PACKET_SHIFT);
    return command(device->host, TRB_TYPE_EVALUATE_CONTEXT, device->input_physical,
                   device->slot << EVENT_SLOT_SHIFT, NULL);
}

static void parse_configuration(struct usb_device *device, uint16_t total) {
    const uint8_t *buffer = device->buffer;
    device->configuration = buffer[5];
    uint8_t class_code = 0, subclass = 0, protocol = 0, interface = 0;
    struct hid_function *function = NULL;
    for (uint16_t offset = 0; offset + 2U <= total;) {
        uint8_t length = buffer[offset];
        uint8_t type = buffer[offset + 1U];
        if (length < 2U || offset + length > total) break;
        if (type == DESCRIPTOR_INTERFACE && length >= 9U) {
            interface = buffer[offset + 2U];
            class_code = buffer[offset + 5U];
            subclass = buffer[offset + 6U];
            protocol = buffer[offset + 7U];
            function = NULL;
            if (device->interface_count < 8U) {
                device->interfaces[device->interface_count][0] = class_code;
                device->interfaces[device->interface_count][1] = subclass;
                device->interfaces[device->interface_count][2] = protocol;
                device->interface_count++;
            }
            if (class_code == CLASS_HID && device->hid_count < MAX_HID) {
                function = &device->hid[device->hid_count];
                memset(function, 0, sizeof(*function));
                function->protocol = subclass == HID_SUBCLASS_BOOT && protocol == HID_KEYBOARD
                                         ? HID_KEYBOARD : HID_MOUSE;
                function->subclass = subclass;
                function->interface_protocol = protocol;
                function->interface = interface;
            }
            if (class_code == CLASS_HUB) device->hub = 1;
            if (class_code == CLASS_STORAGE && subclass == STORAGE_SUBCLASS_SCSI &&
                protocol == STORAGE_BULK_ONLY && !device->storage_interface &&
                !device->bulk_in_address)
                device->storage_interface = (uint8_t)(interface | 0x80U);
        } else if (type == DESCRIPTOR_HID && length >= 9U && function) {
            function->descriptor_length = (uint16_t)(buffer[offset + 7U] | (buffer[offset + 8U] << 8));
        } else if (type == DESCRIPTOR_ENDPOINT && length >= 7U) {
            uint8_t address = buffer[offset + 2U];
            uint8_t attributes = buffer[offset + 3U];
            uint16_t packet = (uint16_t)((buffer[offset + 4U] | (buffer[offset + 5U] << 8)) & 0x7FFU);
            uint8_t interval = buffer[offset + 6U];
            uint8_t kind = attributes & ENDPOINT_TYPE_MASK;
            if (function && !function->address && kind == ENDPOINT_TYPE_INTERRUPT &&
                (address & ENDPOINT_DIRECTION_IN)) {
                function->address = address;
                function->packet = packet;
                function->interval = interval;
                function->dci = (uint32_t)(address & ENDPOINT_NUMBER_MASK) * 2U + 1U;
                device->hid_count++;
                function = NULL;
            } else if (class_code == CLASS_HUB && kind == ENDPOINT_TYPE_INTERRUPT &&
                       (address & ENDPOINT_DIRECTION_IN) && !device->hub_address) {
                device->hub_address = address;
                device->hub_packet = packet;
                device->hub_interval = interval;
            } else if (class_code == CLASS_STORAGE && (device->storage_interface & 0x80U) &&
                       (device->storage_interface & 0x7FU) == interface &&
                       kind == ENDPOINT_TYPE_BULK) {
                if (address & ENDPOINT_DIRECTION_IN) {
                    if (!device->bulk_in_address) {
                        device->bulk_in_address = address;
                        device->bulk_in_packet = packet;
                    }
                } else if (!device->bulk_out_address) {
                    device->bulk_out_address = address;
                    device->bulk_out_packet = packet;
                }
            }
        }
        offset = (uint16_t)(offset + length);
    }
    if (device->device_class == CLASS_HUB) device->hub = 1;
}

static int classify_hid(struct usb_device *device, struct hid_function *function) {
    if (function->protocol == HID_KEYBOARD) return 0;
    uint16_t length = function->descriptor_length;
    if (!length || length > RING_BYTES) length = RING_BYTES;
    memset(device->buffer, 0, RING_BYTES);
    if (control(device, REQUEST_TYPE_STANDARD_INTERFACE_IN, REQUEST_GET_DESCRIPTOR,
                DESCRIPTOR_HID_REPORT << 8, function->interface, length) == 0 &&
        hid_parse_mouse(device->buffer, length, &function->layout) == 0) {
        function->report_mode = 1;
        return 0;
    }
    if (function->subclass == HID_SUBCLASS_BOOT && function->interface_protocol == HID_MOUSE) return 0;
    kprintf("XHCI: %s interface %u is HID but not a keyboard or a relative mouse\n", device->name,
            (unsigned)function->interface);
    return -1;
}

static int start_hid(struct usb_device *device) {
    uint32_t added = 0, entries = 1;
    unsigned kept = 0;
    for (unsigned index = 0; index < device->hid_count; index++) {
        if (classify_hid(device, &device->hid[index]) != 0) continue;
        if (kept != index) device->hid[kept] = device->hid[index];
        kept++;
    }
    device->hid_count = kept;
    if (!kept) return -1;
    clear_input(device);
    for (unsigned index = 0; index < device->hid_count; index++) {
        struct hid_function *function = &device->hid[index];
        if (ring_create(device->host, &function->ring) != 0) return -1;
        function->report = page_for(device->host, &function->report_physical);
        if (!function->report) return -1;
        write_interrupt_endpoint(device, function->dci, &function->ring, function->packet,
                                 function->interval);
        added |= 1U << function->dci;
        if (function->dci > entries) entries = function->dci;
    }
    if (configure(device, added, entries) != 0) return -1;
    for (unsigned index = 0; index < device->hid_count; index++) {
        struct hid_function *function = &device->hid[index];
        if (!function->report_mode || function->subclass == HID_SUBCLASS_BOOT)
            (void)control(device, REQUEST_TYPE_CLASS_INTERFACE, REQUEST_HID_SET_PROTOCOL,
                          function->report_mode ? HID_PROTOCOL_REPORT : 0, function->interface, 0);
        (void)control(device, REQUEST_TYPE_CLASS_INTERFACE, REQUEST_HID_SET_IDLE, 0,
                      function->interface, 0);
        hid_arm(device, function);
    }
    return 0;
}

static int start_storage(struct usb_device *device) {
    if (storage_entries >= (int)MAX_STORAGE) return -1;
    device->bulk_in_dci = (uint32_t)(device->bulk_in_address & ENDPOINT_NUMBER_MASK) * 2U + 1U;
    device->bulk_out_dci = (uint32_t)(device->bulk_out_address & ENDPOINT_NUMBER_MASK) * 2U;
    if (ring_create(device->host, &device->bulk_in) != 0 ||
        ring_create(device->host, &device->bulk_out) != 0) return -1;
    clear_input(device);
    write_bulk_endpoint(device, device->bulk_in_dci, &device->bulk_in, EP_TYPE_BULK_IN,
                        device->bulk_in_packet);
    write_bulk_endpoint(device, device->bulk_out_dci, &device->bulk_out, EP_TYPE_BULK_OUT,
                        device->bulk_out_packet);
    uint32_t entries = device->bulk_in_dci > device->bulk_out_dci ? device->bulk_in_dci
                                                                  : device->bulk_out_dci;
    if (configure(device, (1U << device->bulk_in_dci) | (1U << device->bulk_out_dci), entries) != 0)
        return -1;
    device->storage = storage_entries;
    storage_table[storage_entries].host = device->host;
    storage_table[storage_entries].device = device;
    storage_entries++;
    return 0;
}

static int hub_request(struct usb_device *hub, uint8_t request, uint16_t feature, uint16_t port) {
    return control(hub, REQUEST_TYPE_CLASS_OTHER_OUT, request, feature, port, 0);
}

static int start_hub(struct usb_device *device) {
    if (device->speed >= USB_SPEED_SUPER) {
        kprintf("XHCI: %s is a SuperSpeed hub; only its USB 2 half is followed\n", device->name);
        return -1;
    }
    if (device->depth >= HUB_MAX_DEPTH || !device->hub_address) return -1;
    if (control(device, REQUEST_TYPE_CLASS_DEVICE_IN, REQUEST_GET_DESCRIPTOR,
                DESCRIPTOR_HUB << 8, 0, 9) != 0) return -1;
    device->hub_ports = device->buffer[2];
    if (device->hub_ports > HUB_MAX_PORTS) device->hub_ports = HUB_MAX_PORTS;
    uint16_t characteristics = (uint16_t)(device->buffer[3] | (device->buffer[4] << 8));
    device->hub_think = (characteristics >> 5) & 3U;
    device->hub_power_ms = (uint32_t)device->buffer[5] * 2U;

    device->hub_dci = (uint32_t)(device->hub_address & ENDPOINT_NUMBER_MASK) * 2U + 1U;
    if (ring_create(device->host, &device->hub_ring) != 0) return -1;
    device->hub_bitmap = page_for(device->host, &device->hub_bitmap_physical);
    if (!device->hub_bitmap) return -1;
    if (!device->hub_packet) device->hub_packet = 1U;
    clear_input(device);
    write_interrupt_endpoint(device, device->hub_dci, &device->hub_ring, device->hub_packet,
                             device->hub_interval);
    if (configure(device, 1U << device->hub_dci, device->hub_dci) != 0) return -1;

    for (uint32_t port = 1; port <= device->hub_ports; port++)
        (void)hub_request(device, REQUEST_SET_FEATURE, HUB_FEATURE_PORT_POWER, (uint16_t)port);
    uint64_t power = (uint64_t)(device->hub_power_ms < 100U ? 100U : device->hub_power_ms) * NS_PER_MS;
    pause_ns(power);
    for (uint32_t port = 1; port <= device->hub_ports; port++)
        device->hub_changed |= 1U << port;
    hub_arm(device);
    return 0;
}

static struct usb_device *attach(struct xhci_host *host, struct usb_device *parent,
                                 uint32_t port, uint32_t speed) {
    struct usb_device *device = allocate_device(host);
    if (!device) {
        kprintf("XHCI%u: no room for another device\n", host->index);
        return NULL;
    }
    uint32_t slot = 0;
    if (command(host, TRB_TYPE_ENABLE_SLOT, 0, 0, &slot) != 0 || !slot || slot > host->max_slots) {
        kprintf("XHCI%u: no slot for a device on port %u\n", host->index, (unsigned)port);
        return NULL;
    }
    device->used = 1;
    device->slot = slot;
    device->speed = speed;
    device->parent = parent;
    device->parent_port = port;
    if (parent) {
        device->root_port = parent->root_port;
        device->depth = parent->depth + 1U;
        device->route = parent->route | ((port > 15U ? 15U : port) << (4U * parent->depth));
        if (speed == USB_SPEED_LOW || speed == USB_SPEED_FULL) {
            if (parent->speed == USB_SPEED_HIGH) {
                device->tt_slot = parent->slot;
                device->tt_port = port;
            } else {
                device->tt_slot = parent->tt_slot;
                device->tt_port = parent->tt_port;
            }
        }
    } else {
        device->root_port = port;
    }
    name_device(device);
    device->ep0_packet = speed >= USB_SPEED_SUPER ? 512U : speed == USB_SPEED_HIGH ? 64U : 8U;

    device->input_context = page_for(host, &device->input_physical);
    device->output_context = page_for(host, &device->output_physical);
    device->buffer = page_for(host, &device->buffer_physical);
    if (!device->input_context || !device->output_context || !device->buffer ||
        ring_create(host, &device->ep0) != 0)
        goto fail;

    uint32_t *control_context = context_at(host, device->input_context, INPUT_CONTROL_INDEX);
    control_context[1] = ADD_SLOT | ADD_EP0;
    write_slot_context(device, 1);
    uint32_t *endpoint = context_at(host, device->input_context, 2);
    endpoint[1] = (EP_TYPE_CONTROL << EP_TYPE_SHIFT) | (EP_ERROR_COUNT << EP_ERROR_COUNT_SHIFT) |
                  (device->ep0_packet << EP_MAX_PACKET_SHIFT);
    endpoint[2] = (uint32_t)(device->ep0.physical | EP_DEQUEUE_CYCLE);
    endpoint[3] = (uint32_t)(device->ep0.physical >> 32);
    endpoint[4] = 8U;
    host->dcbaa[slot] = device->output_physical;
    host->by_slot[slot] = device;
    if (command(host, TRB_TYPE_ADDRESS_DEVICE, device->input_physical,
                slot << EVENT_SLOT_SHIFT, NULL) != 0) {
        kprintf("XHCI: %s would not take an address\n", device->name);
        goto fail;
    }
    if (setup_ep0_packet(device) != 0 ||
        control(device, USB_DIRECTION_IN, REQUEST_GET_DESCRIPTOR, DESCRIPTOR_DEVICE << 8, 0, 18) != 0) {
        kprintf("XHCI: %s would not describe itself\n", device->name);
        goto fail;
    }
    device->device_class = device->buffer[4];
    if (control(device, USB_DIRECTION_IN, REQUEST_GET_DESCRIPTOR,
                DESCRIPTOR_CONFIGURATION << 8, 0, 9) != 0) goto fail;
    uint16_t total = (uint16_t)(device->buffer[2] | (device->buffer[3] << 8));
    if (total < 9U) goto fail;
    if (total > RING_BYTES) total = RING_BYTES;
    if (control(device, USB_DIRECTION_IN, REQUEST_GET_DESCRIPTOR,
                DESCRIPTOR_CONFIGURATION << 8, 0, total) != 0) goto fail;
    parse_configuration(device, total);
    if (control(device, 0, REQUEST_SET_CONFIGURATION, device->configuration, 0, 0) != 0) {
        kprintf("XHCI: %s would not take its configuration\n", device->name);
        goto fail;
    }

    if (device->hub) {
        if (start_hub(device) != 0) {
            kprintf("XHCI: %s hub did not start\n", device->name);
            goto fail;
        }
        kprintf("XHCI: hub at %s slot %u, %u ports\n", device->name, (unsigned)slot,
                (unsigned)device->hub_ports);
        device->started = 1;
        return device;
    }
    if (device->hid_count && start_hid(device) == 0) {
        for (unsigned index = 0; index < device->hid_count; index++) {
            struct hid_function *function = &device->hid[index];
            if (function->report_mode)
                kprintf("XHCI: mouse at %s slot %u, endpoint %x, report %u, %u buttons, %u-bit motion\n",
                        device->name, (unsigned)slot, (unsigned)function->address,
                        (unsigned)function->layout.report_id, (unsigned)function->layout.buttons,
                        (unsigned)function->layout.x.size);
            else
                kprintf("XHCI: %s at %s slot %u, endpoint %x reporting\n",
                        function->protocol == HID_KEYBOARD ? "keyboard" : "boot mouse",
                        device->name, (unsigned)slot, (unsigned)function->address);
        }
        device->started = 1;
        return device;
    }
    if (device->bulk_in_address && device->bulk_out_address) {
        if (start_storage(device) != 0) {
            kprintf("XHCI: %s storage did not start\n", device->name);
            goto fail;
        }
        kprintf("XHCI: mass storage at %s slot %u\n", device->name, (unsigned)slot);
        if (booted) usb_host_storage_added(&xhci_usb_host, device->storage);
        device->started = 1;
        return device;
    }
    kprintf("XHCI: %s slot %u is not a device this driver knows (class %u)\n", device->name,
            (unsigned)slot, (unsigned)device->device_class);
    for (unsigned index = 0; index < device->interface_count; index++)
        kprintf("XHCI: %s interface %u: class %u subclass %u protocol %u\n", device->name, index,
                (unsigned)device->interfaces[index][0], (unsigned)device->interfaces[index][1],
                (unsigned)device->interfaces[index][2]);
    device->started = 1;
    return device;

fail:
    remove_device(device);
    return NULL;
}

static struct usb_device *child_on(struct xhci_host *host, struct usb_device *parent,
                                   uint32_t port) {
    for (unsigned index = 0; index < MAX_DEVICES; index++) {
        struct usb_device *device = &host->devices[index];
        if (!device->used || device->parent != parent) continue;
        if (parent ? device->parent_port == port : device->root_port == port) return device;
    }
    return NULL;
}

static uint32_t root_speed(uint32_t portsc) {
    return (portsc >> PORTSC_SPEED_SHIFT) & PORTSC_SPEED_MASK;
}

static int root_reset(struct xhci_host *host, uint32_t port, uint32_t *portsc) {
    uint64_t address = port_register(host, port);
    uint32_t status = read32(address);
    if (status & PORTSC_ENABLED) {
        *portsc = status;
        return 0;
    }
    write32(address, (status & PORTSC_NEUTRAL) | PORTSC_RESET);
    uint64_t deadline = now() + PORT_RESET_TIMEOUT_NS;
    for (;;) {
        status = read32(address);
        if (!(status & PORTSC_RESET) && (status & PORTSC_ENABLED)) break;
        if (!(status & PORTSC_CONNECTED) || now() >= deadline) return -1;
        cpu_relax();
    }
    write32(address, (status & PORTSC_NEUTRAL) | (status & PORTSC_CHANGES));
    pause_ns(RESET_RECOVERY_NS);
    *portsc = read32(address);
    return (*portsc & PORTSC_ENABLED) ? 0 : -1;
}

static void service_root_port(struct xhci_host *host, uint32_t port) {
    uint64_t address = port_register(host, port);
    uint32_t status = read32(address);
    if (status & PORTSC_CHANGES) write32(address, (status & PORTSC_NEUTRAL) | (status & PORTSC_CHANGES));
    struct usb_device *child = child_on(host, NULL, port);
    int connected = (status & PORTSC_CONNECTED) != 0;
    if (child && (!connected || (status & PORTSC_CONNECT_CHANGE))) {
        remove_device(child);
        child = NULL;
    }
    if (!connected) {
        host->root_seen[port] = 0;
        return;
    }
    if (child) return;
    if (!host->root_seen[port]) host->root_seen[port] = now();
    if (now() - host->root_seen[port] < DEBOUNCE_NS) {
        host->root_changed[port / 64U] |= 1ULL << (port % 64U);
        return;
    }
    host->root_seen[port] = 0;
    uint32_t portsc = 0;
    if (root_reset(host, port, &portsc) != 0) {
        kprintf("XHCI%u: port %u would not reset\n", host->index, (unsigned)port);
        return;
    }
    (void)attach(host, NULL, port, root_speed(portsc));
}

static int hub_port_status(struct usb_device *hub, uint32_t port, uint16_t *status, uint16_t *change) {
    if (control(hub, REQUEST_TYPE_CLASS_OTHER_IN, REQUEST_GET_STATUS, 0, (uint16_t)port, 4) != 0)
        return -1;
    *status = (uint16_t)(hub->buffer[0] | (hub->buffer[1] << 8));
    *change = (uint16_t)(hub->buffer[2] | (hub->buffer[3] << 8));
    return 0;
}

static void clear_hub_changes(struct usb_device *hub, uint32_t port, uint16_t change) {
    if (change & HUB_CHANGE_CONNECTION)
        (void)hub_request(hub, REQUEST_CLEAR_FEATURE, HUB_FEATURE_C_CONNECTION, (uint16_t)port);
    if (change & HUB_CHANGE_ENABLE)
        (void)hub_request(hub, REQUEST_CLEAR_FEATURE, HUB_FEATURE_C_ENABLE, (uint16_t)port);
    if (change & HUB_CHANGE_SUSPEND)
        (void)hub_request(hub, REQUEST_CLEAR_FEATURE, HUB_FEATURE_C_SUSPEND, (uint16_t)port);
    if (change & HUB_CHANGE_OVER_CURRENT)
        (void)hub_request(hub, REQUEST_CLEAR_FEATURE, HUB_FEATURE_C_OVER_CURRENT, (uint16_t)port);
    if (change & HUB_CHANGE_RESET)
        (void)hub_request(hub, REQUEST_CLEAR_FEATURE, HUB_FEATURE_C_RESET, (uint16_t)port);
}

static void service_hub_port(struct usb_device *hub, uint32_t port) {
    uint16_t status = 0, change = 0;
    if (hub_port_status(hub, port, &status, &change) != 0) return;
    clear_hub_changes(hub, port, change);
    struct xhci_host *host = hub->host;
    struct usb_device *child = child_on(host, hub, port);
    int connected = (status & HUB_STATUS_CONNECTION) != 0;
    if (child && (!connected || (change & HUB_CHANGE_CONNECTION))) {
        remove_device(child);
        child = NULL;
    }
    if (!connected) {
        hub->hub_seen[port] = 0;
        return;
    }
    if (child) return;
    if (!hub->hub_seen[port]) hub->hub_seen[port] = now();
    if (now() - hub->hub_seen[port] < DEBOUNCE_NS) {
        hub->hub_changed |= 1U << port;
        return;
    }
    hub->hub_seen[port] = 0;
    if (hub_request(hub, REQUEST_SET_FEATURE, HUB_FEATURE_PORT_RESET, (uint16_t)port) != 0) return;
    uint64_t deadline = now() + PORT_RESET_TIMEOUT_NS;
    for (;;) {
        pause_ns(10ULL * NS_PER_MS);
        if (hub_port_status(hub, port, &status, &change) != 0) return;
        if (change & HUB_CHANGE_RESET) break;
        if (now() >= deadline) {
            kprintf("XHCI: %s port %u would not reset\n", hub->name, (unsigned)port);
            return;
        }
    }
    clear_hub_changes(hub, port, change);
    pause_ns(RESET_RECOVERY_NS);
    if (!(status & HUB_STATUS_ENABLE)) return;
    uint32_t speed = (status & HUB_STATUS_LOW_SPEED)    ? USB_SPEED_LOW
                     : (status & HUB_STATUS_HIGH_SPEED) ? USB_SPEED_HIGH
                                                        : USB_SPEED_FULL;
    (void)attach(host, hub, port, speed);
}

static void recover_hid(struct usb_device *device, struct hid_function *function) {
    function->needs_recovery = 0;
    if (++function->recoveries > RECOVERY_LIMIT) {
        if (function->recoveries == RECOVERY_LIMIT + 1U)
            kprintf("XHCI: %s endpoint %x keeps failing; left stopped\n", device->name,
                    (unsigned)function->address);
        return;
    }
    if (realign_endpoint(device, function->dci, &function->ring) != 0) return;
    if (function->stalled)
        (void)control(device, REQUEST_TYPE_STANDARD_ENDPOINT, REQUEST_CLEAR_FEATURE,
                      FEATURE_ENDPOINT_HALT, function->address, 0);
    hid_arm(device, function);
}

static void recover_hub(struct usb_device *device) {
    device->hub_needs_recovery = 0;
    if (++device->hub_recoveries > RECOVERY_LIMIT) return;
    if (realign_endpoint(device, device->hub_dci, &device->hub_ring) != 0) return;
    (void)control(device, REQUEST_TYPE_STANDARD_ENDPOINT, REQUEST_CLEAR_FEATURE,
                  FEATURE_ENDPOINT_HALT, device->hub_address, 0);
    for (uint32_t port = 1; port <= device->hub_ports; port++) device->hub_changed |= 1U << port;
    hub_arm(device);
}

static void service(struct xhci_host *host) {
    if (!host->present || host->failed) return;
    for (unsigned index = 0; index < MAX_DEVICES; index++) {
        struct usb_device *device = &host->devices[index];
        if (!device->used) continue;
        for (unsigned function = 0; function < device->hid_count; function++)
            if (device->hid[function].needs_recovery) recover_hid(device, &device->hid[function]);
        if (device->hub && device->hub_needs_recovery) recover_hub(device);
    }
    for (uint32_t port = 1; port <= host->max_ports && port < MAX_PORTS; port++) {
        uint64_t bit = 1ULL << (port % 64U);
        if (!(host->root_changed[port / 64U] & bit)) continue;
        host->root_changed[port / 64U] &= ~bit;
        service_root_port(host, port);
    }
    for (unsigned index = 0; index < MAX_DEVICES; index++) {
        struct usb_device *device = &host->devices[index];
        if (!device->used || !device->hub || !device->hub_changed) continue;
        uint32_t changed = device->hub_changed;
        device->hub_changed = 0;
        for (uint32_t port = 1; port <= device->hub_ports; port++)
            if (device->used && (changed & (1U << port))) service_hub_port(device, port);
    }
}

static int any_work(struct xhci_host *host) {
    for (unsigned word = 0; word < 4U; word++)
        if (host->root_changed[word]) return 1;
    for (unsigned index = 0; index < MAX_DEVICES; index++) {
        struct usb_device *device = &host->devices[index];
        if (!device->used) continue;
        if (device->hub && (device->hub_changed || device->hub_needs_recovery)) return 1;
        for (unsigned function = 0; function < device->hid_count; function++)
            if (device->hid[function].needs_recovery) return 1;
    }
    return 0;
}

void xhci_poll(void) {
    for (unsigned index = 0; index < host_count; index++) pump(&hosts[index]);
    if (servicing || !booted) return;
    servicing = 1;
    for (unsigned index = 0; index < host_count; index++)
        if (any_work(&hosts[index])) service(&hosts[index]);
    servicing = 0;
}

static void check_health(struct xhci_host *host) {
    if (host->failed) return;
    if (read32(host->operational + XHCI_USBSTS) & USBSTS_HOST_ERROR) {
        kprintf("XHCI%u: host system error; the controller is stopped\n", host->index);
        host->failed = 1;
    }
}

static void interrupt(void *context) {
    struct xhci_host *host = (struct xhci_host *)context;
    if (!host->present) return;
    uint32_t status = read32(host->operational + XHCI_USBSTS);
    if (status & USBSTS_EVENT_INTERRUPT)
        write32(host->operational + XHCI_USBSTS, USBSTS_EVENT_INTERRUPT);
    uint32_t iman = read32(host->interrupter + XHCI_IMAN);
    if (iman & IMAN_PENDING) write32(host->interrupter + XHCI_IMAN, iman);
    pump(host);
}

static int hid_present(uint8_t protocol) {
    for (unsigned host = 0; host < host_count; host++) {
        for (unsigned index = 0; index < MAX_DEVICES; index++) {
            struct usb_device *device = &hosts[host].devices[index];
            if (!device->used) continue;
            for (unsigned function = 0; function < device->hid_count; function++)
                if (device->hid[function].protocol == protocol) return 1;
        }
    }
    return 0;
}

int xhci_keyboard_present(void) { return hid_present(HID_KEYBOARD); }
int xhci_pointer_present(void) { return hid_present(HID_MOUSE); }

static int storage_count(void) { return storage_entries; }

static int storage_transfer(int index, int in, uint64_t physical, uint32_t length) {
    if (index < 0 || index >= storage_entries || !storage_table[index].device) return -1;
    return bulk(storage_table[index].device, in, physical, length);
}

static int storage_reset(int index) {
    if (index < 0 || index >= storage_entries || !storage_table[index].device) return -1;
    struct usb_device *device = storage_table[index].device;
    if (control(device, REQUEST_TYPE_CLASS_INTERFACE, REQUEST_STORAGE_RESET, 0,
                device->storage_interface & 0x7FU, 0) != 0)
        return -1;
    (void)realign_endpoint(device, device->bulk_in_dci, &device->bulk_in);
    (void)realign_endpoint(device, device->bulk_out_dci, &device->bulk_out);
    (void)control(device, REQUEST_TYPE_STANDARD_ENDPOINT, REQUEST_CLEAR_FEATURE,
                  FEATURE_ENDPOINT_HALT, device->bulk_in_address, 0);
    (void)control(device, REQUEST_TYPE_STANDARD_ENDPOINT, REQUEST_CLEAR_FEATURE,
                  FEATURE_ENDPOINT_HALT, device->bulk_out_address, 0);
    return 0;
}

static int storage_present(int index) {
    return index >= 0 && index < storage_entries && storage_table[index].device != NULL &&
           !storage_table[index].device->disconnected;
}

static const struct usb_host xhci_usb_host = {
    .name = "xhci",
    .storage_count = storage_count,
    .bulk_transfer = storage_transfer,
    .reset_recovery = storage_reset,
    .present = storage_present,
};

static void take_from_firmware(struct xhci_host *host) {
    uint32_t parameters = read32(host->base + XHCI_HCCPARAMS1);
    uint32_t offset = (parameters >> HCCPARAMS1_XECP_SHIFT) * 4U;
    for (unsigned guard = 0; offset && offset < XHCI_REGISTER_BYTES && guard < 64U; guard++) {
        uint64_t capability = host->base + offset;
        uint32_t header = read32(capability);
        if ((header & 0xFFU) == XECP_LEGACY) {
            if (header & LEGACY_BIOS_OWNED) {
                write32(capability, header | LEGACY_OS_OWNED);
                if (wait_bits(capability, LEGACY_BIOS_OWNED | LEGACY_OS_OWNED, LEGACY_OS_OWNED,
                              HANDOFF_TIMEOUT_NS) != 0)
                    kprintf("XHCI%u: firmware kept the controller; taking it anyway\n", host->index);
            }
            uint32_t legacy = read32(capability + 4U);
            write32(capability + 4U, (legacy & ~LEGACY_SMI_ENABLES) | LEGACY_SMI_EVENTS);
            return;
        }
        uint32_t next = (header >> 8) & 0xFFU;
        if (!next) return;
        offset += next * 4U;
    }
}

static int reset_host(struct xhci_host *host) {
    uint64_t operational = host->operational;
    if (wait_bits(operational + XHCI_USBSTS, USBSTS_NOT_READY, 0, RESET_TIMEOUT_NS) != 0) return -1;
    uint32_t value = read32(operational + XHCI_USBCMD);
    if (value & USBCMD_RUN) {
        write32(operational + XHCI_USBCMD, value & ~USBCMD_RUN);
        if (wait_bits(operational + XHCI_USBSTS, USBSTS_HALTED, USBSTS_HALTED, HALT_TIMEOUT_NS) != 0) {
            kprintf("XHCI%u: controller will not halt\n", host->index);
            return -1;
        }
    }
    write32(operational + XHCI_USBCMD, read32(operational + XHCI_USBCMD) | USBCMD_RESET);
    if (wait_bits(operational + XHCI_USBCMD, USBCMD_RESET, 0, RESET_TIMEOUT_NS) != 0 ||
        wait_bits(operational + XHCI_USBSTS, USBSTS_NOT_READY, 0, RESET_TIMEOUT_NS) != 0) {
        kprintf("XHCI%u: reset did not complete\n", host->index);
        return -1;
    }
    return 0;
}

static int build_host(struct xhci_host *host) {
    uint64_t dcbaa_physical = 0;
    host->dcbaa = page_for(host, &dcbaa_physical);
    if (!host->dcbaa) return -1;
    uint32_t structural2 = read32(host->base + XHCI_HCSPARAMS2);
    uint32_t scratchpads = ((structural2 >> 21) & 0x1FU) | (((structural2 >> 27) & 0x1FU) << 5);
    if (scratchpads) {
        uint64_t table_physical = 0;
        uint64_t *table = page_for(host, &table_physical);
        if (!table || scratchpads > RING_BYTES / sizeof(uint64_t)) return -1;
        for (uint32_t index = 0; index < scratchpads; index++) {
            uint64_t buffer = 0;
            if (!page_for(host, &buffer)) return -1;
            table[index] = buffer;
        }
        host->dcbaa[0] = table_physical;
    }
    write32(host->operational + XHCI_CONFIG, host->max_slots);
    write64(host->operational + XHCI_DCBAAP, dcbaa_physical);

    if (ring_create(host, &host->commands) != 0) return -1;
    write64(host->operational + XHCI_CRCR, host->commands.physical | CRCR_RING_CYCLE_STATE);

    host->events.entries = page_for(host, &host->events.physical);
    if (!host->events.entries) return -1;
    host->events.index = 0;
    host->events.cycle = 1;
    uint64_t segment_physical = 0;
    uint32_t *segment = page_for(host, &segment_physical);
    if (!segment) return -1;
    segment[0] = (uint32_t)host->events.physical;
    segment[1] = (uint32_t)(host->events.physical >> 32);
    segment[2] = RING_TRB_COUNT;
    host->interrupter = host->runtime + XHCI_INTERRUPTER0;
    write32(host->interrupter + XHCI_ERSTSZ, 1);
    write64(host->interrupter + XHCI_ERDP, host->events.physical | ERDP_EVENT_HANDLER_BUSY);
    write64(host->interrupter + XHCI_ERSTBA, segment_physical);
    return 0;
}

static void enable_interrupts(struct xhci_host *host) {
    int msix = pci_msix_enable(&host->pci) == 0;
    if (!msix && !pci_find_capability(&host->pci, 0x05U)) return;
    unsigned vector = irq_request("xhci", msix ? "PCI-MSIX" : "PCI-MSI", interrupt, host);
    if (!vector) return;
    if (msix ? pci_msix_bind(&host->pci, 0, vector) != 0 : pci_msi_bind(&host->pci, vector) != 0)
        return;
    host->vector = vector;
    write32(host->interrupter + XHCI_IMOD, 1000U);
    write32(host->interrupter + XHCI_IMAN, IMAN_PENDING | IMAN_ENABLE);
}

static int start_host(struct xhci_host *host) {
    enable_interrupts(host);
    uint32_t value = read32(host->operational + XHCI_USBCMD) | USBCMD_RUN;
    if (host->vector) value |= USBCMD_INTERRUPTS;
    write32(host->operational + XHCI_USBCMD, value);
    if (wait_bits(host->operational + XHCI_USBSTS, USBSTS_HALTED, 0, RESET_TIMEOUT_NS) != 0) {
        kprintf("XHCI%u: controller will not run\n", host->index);
        return -1;
    }
    host->present = 1;
    if (command(host, TRB_TYPE_NO_OP_COMMAND, 0, 0, NULL) != 0) {
        kprintf("XHCI%u: no answer to the no-op command\n", host->index);
        host->present = 0;
        return -1;
    }
    if (host->port_power) {
        for (uint32_t port = 1; port <= host->max_ports; port++) {
            uint64_t address = port_register(host, port);
            uint32_t status = read32(address);
            if (!(status & PORTSC_POWER)) write32(address, (status & PORTSC_NEUTRAL) | PORTSC_POWER);
        }
        pause_ns(PORT_POWER_NS);
    }
    return 0;
}

static int bring_up(struct xhci_host *host) {
    if (host->pci.bar[0] & 1U) return -1;
    uint64_t physical = host->pci.bar[0] & 0xFFFFFFF0U;
    if (((host->pci.bar[0] >> 1) & 3U) == 2U) physical |= (uint64_t)host->pci.bar[1] << 32;
    if (!physical) return -1;
    host->base = vmm_map_device(physical, XHCI_REGISTER_BYTES);
    if (!host->base) {
        kprintf("XHCI%u: could not map registers at %p\n", host->index, (void *)physical);
        return -1;
    }
    pci_enable_bus_mastering(&host->pci);

    uint32_t length_and_version = read32(host->base + XHCI_CAPLENGTH);
    uint8_t capability_length = (uint8_t)length_and_version;
    if (length_and_version == 0xFFFFFFFFU || capability_length < 0x20U || (capability_length & 3U)) {
        kprintf("XHCI%u: registers at %p do not answer\n", host->index, (void *)physical);
        return -1;
    }
    uint32_t structural = read32(host->base + XHCI_HCSPARAMS1);
    uint32_t parameters = read32(host->base + XHCI_HCCPARAMS1);
    host->operational = host->base + capability_length;
    host->runtime = host->base + (read32(host->base + XHCI_RTSOFF) & ~0x1FU);
    host->doorbell = host->base + (read32(host->base + XHCI_DBOFF) & ~0x3U);
    host->max_slots = structural & 0xFFU;
    host->max_ports = (structural >> 24) & 0xFFU;
    host->context_bytes = (parameters & HCCPARAMS1_CONTEXT_64) ? 64U : 32U;
    host->ac64 = (parameters & HCCPARAMS1_AC64) != 0;
    host->port_power = (parameters & HCCPARAMS1_PPC) != 0;
    host->version = (uint16_t)(length_and_version >> XHCI_VERSION_SHIFT);
    if (!host->max_slots || !host->max_ports) {
        kprintf("XHCI%u: controller reports no slots or no ports\n", host->index);
        return -1;
    }
    if (host->max_slots > 255U) host->max_slots = 255U;

    take_from_firmware(host);
    if (reset_host(host) != 0) return -1;
    if (build_host(host) != 0) {
        kprintf("XHCI%u: could not build the controller's rings\n", host->index);
        return -1;
    }
    if (start_host(host) != 0) return -1;
    kprintf("XHCI%u: %x.%x at %x:%x.%x, %u slots, %u ports, %u-byte contexts, %s\n", host->index,
            (unsigned)(host->version >> 8), (unsigned)(host->version & 0xFFU),
            (unsigned)host->pci.bus, (unsigned)host->pci.slot, (unsigned)host->pci.function,
            (unsigned)host->max_slots, (unsigned)host->max_ports, (unsigned)host->context_bytes,
            host->vector ? "interrupts" : "polled");
    return 0;
}

int xhci_init(void) {
    struct pci_device device;
    for (unsigned nth = 0; host_count < MAX_CONTROLLERS; nth++) {
        if (pci_find_nth_class(PCI_CLASS_SERIAL_BUS, PCI_SUBCLASS_USB, nth, &device) != 0) break;
        if (device.prog_if != PCI_PROG_IF_XHCI) continue;
        struct xhci_host *host = &hosts[host_count];
        memset(host, 0, sizeof(*host));
        host->index = host_count;
        host->pci = device;
        if (bring_up(host) != 0) {
            host->present = 0;
            continue;
        }
        host_count++;
    }
    if (!host_count) return -1;

    uint64_t settle = now() + FIRST_SCAN_NS;
    while (now() < settle) {
        for (unsigned index = 0; index < host_count; index++) pump(&hosts[index]);
        cpu_relax();
    }
    for (unsigned index = 0; index < host_count; index++) {
        struct xhci_host *host = &hosts[index];
        for (uint32_t port = 1; port <= host->max_ports && port < MAX_PORTS; port++) {
            if (!(read32(port_register(host, port)) & PORTSC_CONNECTED)) continue;
            host->root_seen[port] = now() - DEBOUNCE_NS;
            service_root_port(host, port);
        }
        for (unsigned round = 0; round < HUB_MAX_DEPTH + 1U; round++) {
            int pending = 0;
            for (unsigned slot = 0; slot < MAX_DEVICES; slot++) {
                struct usb_device *hub = &host->devices[slot];
                if (!hub->used || !hub->hub || !hub->hub_changed) continue;
                pending = 1;
                uint32_t changed = hub->hub_changed;
                hub->hub_changed = 0;
                for (uint32_t port = 1; port <= hub->hub_ports; port++) {
                    if (!(changed & (1U << port))) continue;
                    hub->hub_seen[port] = now() - DEBOUNCE_NS;
                    service_hub_port(hub, port);
                }
            }
            if (!pending) break;
        }
    }
    int devices = 0;
    for (unsigned index = 0; index < host_count; index++)
        for (unsigned slot = 0; slot < MAX_DEVICES; slot++)
            if (hosts[index].devices[slot].used) devices++;
    if (!devices) kprintf("XHCI: no devices attached\n");
    usb_register_host(&xhci_usb_host);
    booted = 1;
    return 0;
}
