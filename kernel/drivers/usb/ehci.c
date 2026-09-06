/* EHCI, the USB 2.0 host controller, and on a BIOS machine the only one there
   is: without it the stick the bootloader read the kernel from is invisible to
   the kernel itself. High-speed mass storage only -- anything slower goes to
   the companion controller -- and the asynchronous schedule only, which has no
   doorbell, so a transfer ends in a poll on the descriptor's status byte. */
#include <stdint.h>
#include <stddef.h>

#include "../../include/pci.h"
#include "../../include/pmm.h"
#include "../../include/vmm.h"
#include "../../include/time.h"
#include "../../include/kstring.h"
#include "../../include/usb.h"
#include "../../include/ehci.h"

extern void kprintf(const char *fmt, ...);

#define PCI_CLASS_SERIAL_BUS 0x0CU
#define PCI_SUBCLASS_USB 0x03U
#define PCI_PROG_IF_EHCI 0x20U

#define PCI_BAR_IO 0x1U
#define PCI_BAR_TYPE_MASK 0x6U
#define PCI_BAR_TYPE_64BIT 0x4U
#define PCI_BAR_ADDRESS_MASK 0xFFFFFFF0U

/* Capability registers. CAPLENGTH is a byte and HCIVERSION the halfword above
   it, so both come out of one aligned dword. */
#define EHCI_CAPLENGTH 0x00U
#define EHCI_HCSPARAMS 0x04U
#define EHCI_HCCPARAMS 0x08U

#define HCSPARAMS_PORTS_MASK 0x0FU
#define HCCPARAMS_EECP_SHIFT 8U
#define HCCPARAMS_EECP_MASK 0xFFU

/* Operational registers, at CAPLENGTH from the base. */
#define EHCI_USBCMD 0x00U
#define EHCI_USBSTS 0x04U
#define EHCI_USBINTR 0x08U
#define EHCI_CTRLDSSEGMENT 0x10U
#define EHCI_PERIODICLISTBASE 0x14U
#define EHCI_ASYNCLISTADDR 0x18U
#define EHCI_CONFIGFLAG 0x40U
#define EHCI_PORTSC 0x44U

#define USBCMD_RUN (1U << 0)
#define USBCMD_RESET (1U << 1)
#define USBCMD_ASYNC_ENABLE (1U << 5)
#define USBCMD_ASYNC_DOORBELL (1U << 6)
#define USBCMD_INTERRUPT_THRESHOLD_SHIFT 16U

#define USBSTS_ASYNC_ADVANCE (1U << 5)
#define USBSTS_HALTED (1U << 12)
#define USBSTS_ASYNC_RUNNING (1U << 15)

#define PORTSC_CONNECTED (1U << 0)
#define PORTSC_CONNECT_CHANGE (1U << 1)
#define PORTSC_ENABLED (1U << 2)
#define PORTSC_ENABLE_CHANGE (1U << 3)
#define PORTSC_OVERCURRENT_CHANGE (1U << 5)
#define PORTSC_RESET (1U << 8)
#define PORTSC_LINE_STATUS_SHIFT 10U
#define PORTSC_LINE_STATUS_MASK 0x3U
#define PORTSC_LINE_STATUS_LOW_SPEED 1U
#define PORTSC_POWER (1U << 12)
#define PORTSC_OWNER (1U << 13)
/* The bits that acknowledge a change by being written back as one. Writing the
   register without masking these off clears changes that were never read. */
#define PORTSC_CHANGE_BITS (PORTSC_CONNECT_CHANGE | PORTSC_ENABLE_CHANGE | PORTSC_OVERCURRENT_CHANGE)

/* The legacy-support capability in PCI config space, found through HCCPARAMS.
   On real hardware the firmware owns the controller until this handshake is
   done, and a driver that skips it is fighting the BIOS for every register. */
#define EHCI_LEGACY_CAPABILITY_ID 0x01U
#define LEGACY_BIOS_OWNED (1U << 16)
#define LEGACY_OS_OWNED (1U << 24)

/* Queue head and descriptor pointers carry their type in the low bits, so
   every link is an address with flags in the bottom five. */
#define LINK_TERMINATE (1U << 0)
#define LINK_TYPE_QH (1U << 1)

#define QTD_STATUS_ACTIVE (1U << 7)
#define QTD_STATUS_HALTED (1U << 6)
#define QTD_STATUS_ERROR_MASK 0x7CU
#define QTD_PID_SHIFT 8U
#define QTD_PID_OUT 0U
#define QTD_PID_IN 1U
#define QTD_PID_SETUP 2U
#define QTD_ERROR_COUNT_SHIFT 10U
#define QTD_INTERRUPT_ON_COMPLETE (1U << 15)
#define QTD_LENGTH_SHIFT 16U
#define QTD_DATA_TOGGLE (1U << 31)

#define QH_ENDPOINT_SHIFT 8U
#define QH_SPEED_HIGH (2U << 12)
#define QH_DATA_TOGGLE_CONTROL (1U << 14)
#define QH_HEAD_OF_LIST (1U << 15)
#define QH_MAX_PACKET_SHIFT 16U
#define QH_RELOAD_SHIFT 28U
#define QH_MULT_SHIFT 30U

#define USB_REQUEST_GET_DESCRIPTOR 0x06U
#define USB_REQUEST_SET_ADDRESS 0x05U
#define USB_REQUEST_SET_CONFIGURATION 0x09U
#define USB_REQUEST_CLEAR_FEATURE 0x01U
#define USB_FEATURE_ENDPOINT_HALT 0x00U
#define USB_DESCRIPTOR_DEVICE 0x01U
#define USB_DESCRIPTOR_CONFIGURATION 0x02U

#define USB_CLASS_MASS_STORAGE 0x08U
#define USB_SUBCLASS_SCSI 0x06U
#define USB_PROTOCOL_BULK_ONLY 0x50U

#define RESET_TIMEOUT_NS (1000ULL * 1000ULL * 1000ULL)
#define PORT_RESET_HOLD_NS (50ULL * 1000ULL * 1000ULL)
#define PORT_ENABLE_TIMEOUT_NS (200ULL * 1000ULL * 1000ULL)
/* How long a bulk transfer may take, and how long it may make no progress.
   A stick committing a write NAKs for longer than two seconds, and giving up
   there abandons a transaction the device is still in -- which stalls the
   endpoint the next wrapper goes into. Ten seconds while the controller is
   working on it, two while the overlay is idle. */
#define TRANSFER_TIMEOUT_NS (10000ULL * 1000ULL * 1000ULL)
#define TRANSFER_QUIET_NS (2000ULL * 1000ULL * 1000ULL)
/* Enumeration is allowed far less patience than a disk transfer. A device that
   is not going to answer a control request has already not answered it, and
   there can be a great many of these: every port of every hub of every
   controller. At two seconds each, a machine full of empty sockets takes
   minutes to decide there is nothing on them. */
#define CONTROL_TIMEOUT_NS (300ULL * 1000ULL * 1000ULL)
#define HANDOFF_TIMEOUT_NS (1000ULL * 1000ULL * 1000ULL)
/* The specification's recovery time after a port reset, before the device is
   required to answer on its default address. */
#define RESET_RECOVERY_NS (20ULL * 1000ULL * 1000ULL)
/* The debounce a connection needs before PORTSC means anything. */
#define PORT_POWER_SETTLE_NS (100ULL * 1000ULL * 1000ULL)
#define SET_ADDRESS_RECOVERY_NS (10ULL * 1000ULL * 1000ULL)

#define EHCI_REGISTER_BYTES 0x1000U
#define MAX_PORTS 15U
#define MAX_DEVICES 8U
/* Four ring heads fit in the front of the DMA page, and no chipset has more
   than the two an old Intel one splits its ports across. */
#define MAX_CONTROLLERS 4U
#define CONFIGURATION_BYTES 512U
/* Enough to name the failure, not enough to bury the log. */
#define BULK_FAILURES_REPORTED 8U
/* And one in every this many after that, so a long failure still says what it
   is doing rather than going quiet. */
#define BULK_FAILURE_INTERVAL 64U

struct ehci_qtd {
    uint32_t next;
    uint32_t alternate;
    uint32_t token;
    uint32_t buffer[5];
    uint32_t buffer_high[5];
    /* Padding to a whole multiple of the 32-byte alignment the controller
       requires, so an array of these can live in one page and every entry
       still satisfies it. */
    uint32_t reserved[3];
};

struct ehci_qh {
    uint32_t horizontal;
    uint32_t characteristics;
    uint32_t capabilities;
    uint32_t current_qtd;
    /* The overlay: the controller copies the active descriptor in here and
       writes its progress back, so these are its scratch space, not ours. */
    uint32_t overlay_next;
    uint32_t overlay_alternate;
    uint32_t overlay_token;
    uint32_t overlay_buffer[5];
    uint32_t overlay_buffer_high[5];
    uint32_t reserved[4];
};

struct ehci_device {
    int used;
    int is_storage;
    uint8_t address;
    uint8_t configuration;
    uint16_t max_packet;
    uint8_t interface;
    uint8_t bulk_in_endpoint;
    uint8_t bulk_out_endpoint;
    uint16_t bulk_in_packet;
    uint16_t bulk_out_packet;
    /* One toggle per endpoint, kept here rather than in the queue head: the
       queue head is rewritten for every transfer and the toggle is not. */
    uint8_t bulk_in_toggle;
    uint8_t bulk_out_toggle;
};

/* The carve-up in ehci_init() depends on these, and getting it wrong is a
   silent overlap the controller answers by ignoring the schedule. */
typedef char ehci_qh_size_check[(sizeof(struct ehci_qh) <= 0x100) ? 1 : -1];
typedef char ehci_qtd_size_check[(3 * sizeof(struct ehci_qtd) <= 0x100) ? 1 : -1];

/* One of these per controller: an old Intel chipset splits its ports across
   two, so finding one and stopping is the same as not looking. The descriptors
   and buffers are shared, because a transfer here is synchronous. */
struct ehci {
    int present;
    uint16_t version;
    uint64_t base;
    uint64_t operational;
    unsigned ports;
    struct ehci_qh *async_head;
    struct ehci_qh *work_qh;
    /* What the working queue head already describes, so a transfer to the same
       endpoint as the last one needs no doorbell. */
    uint32_t work_characteristics;
    uint32_t work_capabilities;
    struct ehci_device devices[MAX_DEVICES];
    unsigned device_count;
};

static struct ehci controllers[MAX_CONTROLLERS];
static unsigned controller_count;

/* One page holds every structure the controllers read. */
static uint8_t *dma_page;
static uint64_t dma_physical;
static struct ehci_qtd *qtds;
static uint8_t *setup_buffer;
static uint8_t *descriptor_buffer;

/* A word the controller reads or writes by itself. Volatile, because the queue
   head is rewritten in a fixed order and that order is the whole interlock:
   the compiler deleted the store that made it inert as dead, and a live queue
   head rewritten under the controller is a transfer that never starts. */
static inline void dma_store32(uint32_t *field, uint32_t value) {
    *(volatile uint32_t *)field = value;
}

static inline uint32_t dma_load32(const uint32_t *field) {
    return *(const volatile uint32_t *)field;
}

static inline uint32_t mmio_read32(uint64_t address) {
    return *(volatile uint32_t *)address;
}

static inline void mmio_write32(uint64_t address, uint32_t value) {
    *(volatile uint32_t *)address = value;
}

static uint64_t operational(const struct ehci *host, uint32_t offset) {
    return host->operational + offset;
}

static uint64_t port_register(const struct ehci *host, unsigned port) {
    return host->operational + EHCI_PORTSC + port * 4U;
}

/* Spin until the masked bits reach `wanted`, or the deadline passes. */
static int wait_for(uint64_t address, uint32_t mask, uint32_t wanted,
                    uint64_t timeout_ns) {
    uint64_t deadline = time_uptime_ns() + timeout_ns;
    for (;;) {
        if ((mmio_read32(address) & mask) == wanted) return 0;
        if (time_uptime_ns() >= deadline) return -1;
        __asm__ volatile("pause");
    }
}

static void delay_ns(uint64_t nanoseconds) {
    uint64_t deadline = time_uptime_ns() + nanoseconds;
    while (time_uptime_ns() < deadline) __asm__ volatile("pause");
}

/* One page of DMA memory below 4 GiB: every pointer the controller follows is
   32 bits over a shared CTRLDSSEGMENT, which stays zero. */
#define DMA_ALLOCATION_ATTEMPTS 64
#define DMA_LIMIT 0x100000000ULL

static void *dma_alloc_page(uint64_t *physical_out) {
    void *rejected[DMA_ALLOCATION_ATTEMPTS];
    unsigned rejects = 0;
    void *taken = NULL;

    for (unsigned attempt = 0; attempt < DMA_ALLOCATION_ATTEMPTS; attempt++) {
        void *page = pmm_alloc_page();
        if (!page) break;
        if ((uint64_t)page < DMA_LIMIT) {
            taken = page;
            break;
        }
        rejected[rejects++] = page;
    }
    while (rejects) pmm_free_page(rejected[--rejects]);
    if (!taken) return NULL;

    void *virtual_address = vmm_phys_to_virt((uint64_t)taken);
    if (!virtual_address) {
        pmm_free_page(taken);
        return NULL;
    }
    memset(virtual_address, 0, 4096);
    *physical_out = (uint64_t)taken;
    return virtual_address;
}

static uint32_t physical_of(const void *within_dma_page) {
    uint64_t offset = (uint64_t)((const uint8_t *)within_dma_page - dma_page);
    return (uint32_t)(dma_physical + offset);
}

/* Take the controller from the firmware: until this handshake completes the
   BIOS still owns it and undoes what is written. Optional, so a HCCPARAMS
   naming no pointer means there is nothing to take. */
static void release_from_firmware(const struct pci_device *device,
                                  uint32_t capabilities) {
    uint8_t pointer = (uint8_t)((capabilities >> HCCPARAMS_EECP_SHIFT) &
                                HCCPARAMS_EECP_MASK);
    if (pointer < 0x40U) return;

    uint32_t legacy = pci_config_read32(device->bus, device->slot,
                                        device->function, pointer);
    if ((uint8_t)legacy != EHCI_LEGACY_CAPABILITY_ID) return;
    if (!(legacy & LEGACY_BIOS_OWNED)) return;

    pci_config_write32(device->bus, device->slot, device->function, pointer,
                       legacy | LEGACY_OS_OWNED);
    uint64_t deadline = time_uptime_ns() + HANDOFF_TIMEOUT_NS;
    for (;;) {
        legacy = pci_config_read32(device->bus, device->slot, device->function,
                                   pointer);
        if (!(legacy & LEGACY_BIOS_OWNED)) return;
        if (time_uptime_ns() >= deadline) break;
        __asm__ volatile("pause");
    }
    /* The firmware did not let go. Taking it anyway is what every other
       operating system does here, and is better than having no disk. */
    kprintf("EHCI: firmware did not release the controller, taking it\n");
    pci_config_write32(device->bus, device->slot, device->function, pointer,
                       (legacy & ~LEGACY_BIOS_OWNED) | LEGACY_OS_OWNED);
}

static int reset_controller(struct ehci *host) {
    /* Stop first: resetting a running controller is undefined. */
    uint32_t command = mmio_read32(operational(host, EHCI_USBCMD));
    mmio_write32(operational(host, EHCI_USBCMD), command & ~USBCMD_RUN);
    if (wait_for(operational(host, EHCI_USBSTS), USBSTS_HALTED, USBSTS_HALTED,
                 RESET_TIMEOUT_NS) != 0) {
        kprintf("EHCI: controller would not halt\n");
        return -1;
    }

    mmio_write32(operational(host, EHCI_USBCMD), USBCMD_RESET);
    if (wait_for(operational(host, EHCI_USBCMD), USBCMD_RESET, 0, RESET_TIMEOUT_NS) != 0) {
        kprintf("EHCI: controller would not reset\n");
        return -1;
    }
    return 0;
}

/*
 * The asynchronous schedule: a ring of one empty queue head that points at
 * itself and is marked the head of the reclamation list. Work is done by
 * linking a second queue head in behind it; the controller walks the ring for
 * as long as the schedule is enabled, with no doorbell to ring.
 */
static void build_async_ring(struct ehci *host) {
    struct ehci_qh *async_head = host->async_head;
    struct ehci_qh *work = host->work_qh;

    memset(async_head, 0, sizeof(*async_head));
    memset(work, 0, sizeof(*work));

    /* Two queue heads in a ring, and neither ever leaves it: an unlinked one
       may not be touched until the async-advance doorbell is acknowledged,
       and an idle queue head with no descriptors is skipped anyway. */
    async_head->horizontal = physical_of(work) | LINK_TYPE_QH;
    async_head->characteristics = QH_HEAD_OF_LIST | QH_SPEED_HIGH |
                                  (64U << QH_MAX_PACKET_SHIFT);
    async_head->capabilities = (1U << QH_MULT_SHIFT);
    async_head->overlay_next = LINK_TERMINATE;
    async_head->overlay_alternate = LINK_TERMINATE;

    work->horizontal = physical_of(async_head) | LINK_TYPE_QH;
    work->overlay_next = LINK_TERMINATE;
    work->overlay_alternate = LINK_TERMINATE;
    host->work_characteristics = 0;
    host->work_capabilities = 0;
}

static int start_controller(struct ehci *host) {
    mmio_write32(operational(host, EHCI_USBINTR), 0);
    mmio_write32(operational(host, EHCI_CTRLDSSEGMENT), 0);
    mmio_write32(operational(host, EHCI_PERIODICLISTBASE), 0);
    mmio_write32(operational(host, EHCI_ASYNCLISTADDR),
                 physical_of(host->async_head));

    uint32_t command = (8U << USBCMD_INTERRUPT_THRESHOLD_SHIFT) |
                       USBCMD_ASYNC_ENABLE | USBCMD_RUN;
    mmio_write32(operational(host, EHCI_USBCMD), command);
    if (wait_for(operational(host, EHCI_USBSTS), USBSTS_HALTED, 0, RESET_TIMEOUT_NS) != 0) {
        kprintf("EHCI: controller would not start\n");
        return -1;
    }
    /* Enabling the schedule and the schedule running are two different
       things, and a transfer queued in between is one the controller never
       walks. */
    if (wait_for(operational(host, EHCI_USBSTS), USBSTS_ASYNC_RUNNING,
                 USBSTS_ASYNC_RUNNING, RESET_TIMEOUT_NS) != 0) {
        kprintf("EHCI: the asynchronous schedule would not start\n");
        return -1;
    }
    /* Route every port to this controller rather than to the companion. Until
       this is written the ports belong to UHCI/OHCI and read as empty. */
    mmio_write32(operational(host, EHCI_CONFIGFLAG), 1U);

    /* Power every port, and only then wait: an unpowered port reports no
       connection, so asking what is plugged into one first has a single
       possible answer. The 100 ms debounce is waited out once, in ehci_init(). */
    for (unsigned port = 0; port < host->ports && port < MAX_PORTS; port++) {
        uint32_t status = mmio_read32(port_register(host, port));
        /* Port Owner goes with it. The firmware hands ports to the companion
           controller for its own legacy emulation and a reset does not always
           take them back; a port left owned elsewhere reads as empty here. If
           what is on it turns out to be full speed, reset_port() gives it
           away again deliberately. */
        mmio_write32(port_register(host, port),
                     (status & ~(PORTSC_CHANGE_BITS | PORTSC_OWNER)) | PORTSC_POWER);
    }
    return 0;
}

/* Hand the port to the companion controller. It is a one-way door: the port
   stops answering here, which is the intent. */
static void release_port(struct ehci *host, unsigned port) {
    uint32_t status = mmio_read32(port_register(host, port));
    mmio_write32(port_register(host, port),
                 (status & ~PORTSC_CHANGE_BITS) | PORTSC_OWNER);
}

/*
 * Reset one port and report whether a high-speed device came up on it.
 *
 * The line status before the reset already names a low-speed device, and after
 * the reset the port enabling itself is what says the device is high speed: a
 * full-speed one leaves the port disabled. Both cases belong to the companion.
 */
static int reset_port(struct ehci *host, unsigned port) {
    uint32_t status = mmio_read32(port_register(host, port));
    if (!(status & PORTSC_CONNECTED)) return -1;

    if (((status >> PORTSC_LINE_STATUS_SHIFT) & PORTSC_LINE_STATUS_MASK) ==
        PORTSC_LINE_STATUS_LOW_SPEED) {
        release_port(host, port);
        return -1;
    }

    status = mmio_read32(port_register(host, port));
    mmio_write32(port_register(host, port),
                 (status & ~(PORTSC_CHANGE_BITS | PORTSC_ENABLED)) | PORTSC_RESET);
    delay_ns(PORT_RESET_HOLD_NS);

    status = mmio_read32(port_register(host, port));
    mmio_write32(port_register(host, port), status & ~(PORTSC_CHANGE_BITS | PORTSC_RESET));
    /* The controller clears the reset bit itself once the signalling is over,
       and only then decides whether to enable the port. */
    if (wait_for(port_register(host, port), PORTSC_RESET, 0, PORT_ENABLE_TIMEOUT_NS) != 0)
        return -1;
    delay_ns(RESET_RECOVERY_NS);

    status = mmio_read32(port_register(host, port));
    if (!(status & PORTSC_ENABLED)) {
        release_port(host, port);
        return -1;
    }
    return 0;
}

/* --- transfers ------------------------------------------------------------ */

/*
 * Fill in one transfer descriptor. A buffer crosses page boundaries by way of
 * the five pointers, each covering the page it points into, so a transfer of
 * up to five pages needs no scattering above.
 */
static void build_qtd(struct ehci_qtd *qtd, uint32_t pid, uint64_t physical,
                      uint32_t length, int toggle) {
    for (unsigned page = 0; page < 5U; page++) {
        dma_store32(&qtd->buffer[page], 0);
        dma_store32(&qtd->buffer_high[page], 0);
    }
    dma_store32(&qtd->next, LINK_TERMINATE);
    dma_store32(&qtd->alternate, LINK_TERMINATE);

    uint32_t token = (length << QTD_LENGTH_SHIFT) | (3U << QTD_ERROR_COUNT_SHIFT) |
                     (pid << QTD_PID_SHIFT) | QTD_STATUS_ACTIVE |
                     QTD_INTERRUPT_ON_COMPLETE;
    if (toggle) token |= QTD_DATA_TOGGLE;

    uint64_t address = physical;
    uint64_t end = physical + length;
    for (unsigned page = 0; length && page < 5U && address < end; page++) {
        dma_store32(&qtd->buffer[page], (uint32_t)address);
        address = (address & ~0xFFFULL) + 0x1000ULL;
    }
    /* Last, so the descriptor is complete before it is armed. */
    dma_store32(&qtd->token, token);
}

/* Make the controller look at the schedule again. EHCI stops walking an async
   schedule with nothing in it and there is no doorbell, so work put into a
   queue head already on the ring can simply never start; turning the schedule
   off and on is what restarts the traversal. */
#define ASYNC_KICK_AFTER_NS (20ULL * 1000ULL * 1000ULL)
#define ASYNC_KICK_TIMEOUT_NS (100ULL * 1000ULL * 1000ULL)

/* Wait for the controller to let go of the queue heads it has cached. Rung
   only when a transfer is abandoned: it may still be talking to the device
   about a descriptor whose buffer is about to be reused. */
#define ASYNC_ADVANCE_TIMEOUT_NS (10ULL * 1000ULL * 1000ULL)

static void async_advance(struct ehci *host) {
    uint32_t command = mmio_read32(operational(host, EHCI_USBCMD));
    if (!(command & USBCMD_ASYNC_ENABLE) || !(command & USBCMD_RUN)) return;
    mmio_write32(operational(host, EHCI_USBSTS), USBSTS_ASYNC_ADVANCE);
    mmio_write32(operational(host, EHCI_USBCMD), command | USBCMD_ASYNC_DOORBELL);
    (void)wait_for(operational(host, EHCI_USBSTS), USBSTS_ASYNC_ADVANCE,
                   USBSTS_ASYNC_ADVANCE, ASYNC_ADVANCE_TIMEOUT_NS);
    mmio_write32(operational(host, EHCI_USBSTS), USBSTS_ASYNC_ADVANCE);
}

static void async_kick(struct ehci *host) {
    uint32_t command = mmio_read32(operational(host, EHCI_USBCMD));
    mmio_write32(operational(host, EHCI_USBCMD), command & ~USBCMD_ASYNC_ENABLE);
    (void)wait_for(operational(host, EHCI_USBSTS), USBSTS_ASYNC_RUNNING, 0,
                   ASYNC_KICK_TIMEOUT_NS);
    mmio_write32(operational(host, EHCI_USBCMD), command | USBCMD_ASYNC_ENABLE);
    (void)wait_for(operational(host, EHCI_USBSTS), USBSTS_ASYNC_RUNNING,
                   USBSTS_ASYNC_RUNNING, ASYNC_KICK_TIMEOUT_NS);
}

/* Point the working queue head at a chain of descriptors and wait for the last
   one to go inactive. Rewritten rather than reused, and unlinked at the end,
   because every transfer here is synchronous. */
static int run_qtds(struct ehci *host, struct ehci_device *device,
                    uint8_t endpoint, uint16_t max_packet, int is_control,
                    struct ehci_qtd *first, struct ehci_qtd *last,
                    uint64_t timeout_ns) {
    struct ehci_qh *work_qh = host->work_qh;

    /* Inert first, and in this order: with nothing to follow, the next pass
       walks past the queue head instead of starting a transfer out of a
       half-written one. */
    dma_store32(&work_qh->overlay_next, LINK_TERMINATE);
    dma_store32(&work_qh->overlay_token, 0);
    dma_store32(&work_qh->overlay_alternate, LINK_TERMINATE);
    dma_store32(&work_qh->current_qtd, 0);

    /* The control-endpoint flag is for full- and low-speed endpoints only.
       Setting it on a high-speed one, which is all this driver talks to, makes
       the controller run a protocol the device is not speaking. */
    (void)is_control;
    uint32_t characteristics = device->address |
                               ((uint32_t)endpoint << QH_ENDPOINT_SHIFT) |
                               QH_SPEED_HIGH | QH_DATA_TOGGLE_CONTROL |
                               ((uint32_t)max_packet << QH_MAX_PACKET_SHIFT) |
                               (3U << QH_RELOAD_SHIFT);
    uint32_t capabilities = (1U << QH_MULT_SHIFT);
    /* Which endpoint the queue head describes is the one thing the controller
       may be holding a copy of, so it is written only when it changes. */
    if (characteristics != host->work_characteristics ||
        capabilities != host->work_capabilities) {
        dma_store32(&work_qh->characteristics, characteristics);
        dma_store32(&work_qh->capabilities, capabilities);
        host->work_characteristics = characteristics;
        host->work_capabilities = capabilities;
    }

    /* Last, and what starts the transfer. */
    dma_store32(&work_qh->overlay_next, physical_of(first));

    uint64_t started = time_uptime_ns();
    uint64_t deadline = started + timeout_ns;
    uint64_t kick_at = started + ASYNC_KICK_AFTER_NS;
    /* The overlay is the controller's own working state, so a change in it is
       what "still going" means -- a device that NAKs leaves it alone but keeps
       the active bit set, which is the case worth waiting out. */
    uint64_t quiet_since = started;
    uint32_t seen_overlay = dma_load32(&work_qh->overlay_token);
    int kicked = 0;
    int status = -1;
    int abandoned = 0;
    for (;;) {
        uint32_t token = dma_load32(&last->token);
        if (!(token & QTD_STATUS_ACTIVE)) {
            status = (token & QTD_STATUS_ERROR_MASK) ? -1 : 0;
            break;
        }
        uint32_t overlay = dma_load32(&work_qh->overlay_token);
        if (overlay & QTD_STATUS_HALTED) break;
        uint64_t now = time_uptime_ns();
        if (overlay != seen_overlay) {
            seen_overlay = overlay;
            quiet_since = now;
        }
        if (now >= deadline ||
            (!(overlay & QTD_STATUS_ACTIVE) && now - quiet_since >= TRANSFER_QUIET_NS)) {
            abandoned = 1;
            break;
        }
        /* Once, and late. Turning the schedule off is not free for a transfer
           that is already under way -- doing it every couple of milliseconds
           stops transfers finishing at all, which is a worse machine than a
           slow one. */
        if (!kicked && now >= kick_at) {
            kicked = 1;
            async_kick(host);
        }
        __asm__ volatile("pause");
    }

    /* Idle again, and still linked. */
    dma_store32(&work_qh->overlay_next, LINK_TERMINATE);
    /* A transfer walked away from is one the controller may still be running,
       and the buffer under it is about to belong to something else. */
    if (abandoned) async_advance(host);
    return status;
}

/*
 * One control transfer on the default pipe: setup, an optional data stage, and
 * a status stage in the opposite direction. The toggle sequence is fixed by
 * the specification rather than tracked, which is why nothing here consults
 * the endpoint toggles the bulk path keeps.
 */
static int control_transfer(struct ehci *host, struct ehci_device *device,
                            uint8_t request_type, uint8_t request,
                            uint16_t value, uint16_t index, uint16_t length,
                            void *data) {
    uint8_t *setup = setup_buffer;
    setup[0] = request_type;
    setup[1] = request;
    setup[2] = (uint8_t)value;
    setup[3] = (uint8_t)(value >> 8);
    setup[4] = (uint8_t)index;
    setup[5] = (uint8_t)(index >> 8);
    setup[6] = (uint8_t)length;
    setup[7] = (uint8_t)(length >> 8);

    int in = (request_type & 0x80U) != 0;
    struct ehci_qtd *setup_qtd = &qtds[0];
    struct ehci_qtd *data_qtd = &qtds[1];
    struct ehci_qtd *status_qtd = &qtds[2];

    build_qtd(setup_qtd, QTD_PID_SETUP, physical_of(setup), 8, 0);
    if (length) {
        build_qtd(data_qtd, in ? QTD_PID_IN : QTD_PID_OUT,
                  physical_of(descriptor_buffer), length, 1);
        setup_qtd->next = physical_of(data_qtd);
        data_qtd->next = physical_of(status_qtd);
    } else {
        setup_qtd->next = physical_of(status_qtd);
    }
    /* The status stage always carries a toggle of one and always runs in the
       direction the data stage did not. */
    build_qtd(status_qtd, in ? QTD_PID_OUT : QTD_PID_IN, 0, 0, 1);

    if (length && !in && data) memcpy(descriptor_buffer, data, length);
    int result = run_qtds(host, device, 0, device->max_packet, 1, setup_qtd,
                          status_qtd, CONTROL_TIMEOUT_NS);
    if (result == 0 && length && in && data) memcpy(data, descriptor_buffer, length);
    return result;
}

/* --- enumeration ---------------------------------------------------------- */

/*
 * Walk a configuration descriptor for a bulk-only mass-storage interface and
 * remember its two bulk endpoints. Everything else is skipped by its own
 * length byte, which is the only safe way through a list whose entries this
 * driver does not all understand.
 */
static int find_storage_interface(struct ehci_device *device,
                                  const uint8_t *buffer, uint16_t total) {
    int in_storage_interface = 0;
    device->bulk_in_endpoint = 0;
    device->bulk_out_endpoint = 0;

    for (uint16_t offset = 0; offset + 2U <= total;) {
        uint8_t length = buffer[offset];
        uint8_t type = buffer[offset + 1U];
        if (length < 2U || offset + length > total) break;

        if (type == 0x04U && length >= 9U) {
            in_storage_interface = buffer[offset + 5U] == USB_CLASS_MASS_STORAGE &&
                                   buffer[offset + 6U] == USB_SUBCLASS_SCSI &&
                                   buffer[offset + 7U] == USB_PROTOCOL_BULK_ONLY;
            /* Kept for the class reset, which is addressed to the interface
               rather than to the device or an endpoint. */
            if (in_storage_interface) device->interface = buffer[offset + 2U];
        } else if (type == 0x05U && length >= 7U && in_storage_interface) {
            uint8_t address = buffer[offset + 2U];
            uint8_t attributes = buffer[offset + 3U];
            uint16_t packet = (uint16_t)(buffer[offset + 4U] |
                                         ((uint16_t)buffer[offset + 5U] << 8));
            if ((attributes & 0x03U) == 0x02U) {
                if (address & 0x80U) {
                    device->bulk_in_endpoint = address & 0x0FU;
                    device->bulk_in_packet = packet;
                } else {
                    device->bulk_out_endpoint = address & 0x0FU;
                    device->bulk_out_packet = packet;
                }
            }
        }
        offset = (uint16_t)(offset + length);
    }
    return (device->bulk_in_endpoint && device->bulk_out_endpoint) ? 0 : -1;
}

/*
 * How a port is named in the log: "1" for a root port, "1.2" for the second
 * port of the hub on root port 1. The packed form is what gets carried around
 * -- root port in the high nibble, the port below it in the low one -- and
 * this is the only place that knows it.
 */
static const char *port_name(unsigned where) {
    static char name[8];
    unsigned root = (where >> 4) & 0xFU;
    unsigned below = where & 0xFU;
    unsigned at = 0;
    if (root >= 10U) name[at++] = (char)('0' + root / 10U);
    name[at++] = (char)('0' + root % 10U);
    if (below) {
        name[at++] = '.';
        if (below >= 10U) name[at++] = (char)('0' + below / 10U);
        name[at++] = (char)('0' + below % 10U);
    }
    name[at] = 0;
    return name;
}

/* Put a device on an address and report what kind it is: the first eight bytes
   of the descriptor carry both the real maximum packet size and the class,
   which is how a hub is known before it is asked for a configuration it has
   not got. `where` is the root port in the high nibble, the hub port in the
   low one. Returns the class, or -1. */
static int address_device(struct ehci *host, struct ehci_device *device,
                          uint8_t address, unsigned where) {
    memset(device, 0, sizeof(*device));
    /* Address zero and the smallest packet size the specification allows,
       until the device has said otherwise. */
    device->address = 0;
    device->max_packet = 64;

    uint8_t header[8];
    if (control_transfer(host, device, 0x80U, USB_REQUEST_GET_DESCRIPTOR,
                         (uint16_t)(USB_DESCRIPTOR_DEVICE << 8), 0, 8,
                         header) != 0) {
        kprintf("EHCI: port %s did not answer GET_DESCRIPTOR\n", port_name(where));
        return -1;
    }
    if (header[7]) device->max_packet = header[7];

    if (control_transfer(host, device, 0x00U, USB_REQUEST_SET_ADDRESS, address,
                         0, 0, NULL) != 0) {
        kprintf("EHCI: port %s refused SET_ADDRESS\n", port_name(where));
        return -1;
    }
    device->address = address;
    delay_ns(SET_ADDRESS_RECOVERY_NS);
    return header[4];
}

/* The configuration descriptor, and the bulk endpoints of a mass-storage
   interface inside it. Every way it can fail says so: a device that is not a
   disk looks exactly like a disk that would not talk. */
static int enumerate_storage(struct ehci *host, struct ehci_device *device,
                             unsigned where) {
    uint8_t header[9];
    if (control_transfer(host, device, 0x80U, USB_REQUEST_GET_DESCRIPTOR,
                         (uint16_t)(USB_DESCRIPTOR_CONFIGURATION << 8), 0, 9,
                         header) != 0) {
        kprintf("EHCI: port %s has no configuration descriptor\n", port_name(where));
        return -1;
    }
    uint16_t total = (uint16_t)(header[2] | ((uint16_t)header[3] << 8));
    if (total > CONFIGURATION_BYTES) total = CONFIGURATION_BYTES;

    static uint8_t configuration[CONFIGURATION_BYTES];
    if (control_transfer(host, device, 0x80U, USB_REQUEST_GET_DESCRIPTOR,
                         (uint16_t)(USB_DESCRIPTOR_CONFIGURATION << 8), 0, total,
                         configuration) != 0) {
        kprintf("EHCI: port %s would not give up its configuration\n", port_name(where));
        return -1;
    }

    if (find_storage_interface(device, configuration, total) != 0) {
        kprintf("EHCI: port %s is not bulk-only mass storage\n", port_name(where));
        return -1;
    }
    device->configuration = header[5];

    if (control_transfer(host, device, 0x00U, USB_REQUEST_SET_CONFIGURATION,
                         device->configuration, 0, 0, NULL) != 0) {
        kprintf("EHCI: port %s refused SET_CONFIGURATION\n", port_name(where));
        return -1;
    }

    device->used = 1;
    device->is_storage = 1;
    kprintf("EHCI: port %s: mass storage at address %u, bulk in %u out %u\n",
            port_name(where), (unsigned)device->address,
            (unsigned)device->bulk_in_endpoint,
            (unsigned)device->bulk_out_endpoint);
    return 0;
}

/* --- hubs -- not optional here: Intel chipsets of the era put a rate-matching
   hub on each EHCI root port and hang every socket off it. Only the management
   is needed, not split transactions; a slower device behind one is skipped. */

#define USB_CLASS_HUB 0x09U
#define HUB_REQUEST_GET_STATUS 0x00U
#define HUB_REQUEST_CLEAR_FEATURE 0x01U
#define HUB_REQUEST_SET_FEATURE 0x03U
#define HUB_DESCRIPTOR_TYPE 0x29U
#define HUB_FEATURE_PORT_RESET 4U
#define HUB_FEATURE_PORT_POWER 8U
#define HUB_FEATURE_C_PORT_RESET 20U
#define HUB_PORT_CONNECTED (1U << 0)
#define HUB_PORT_ENABLED (1U << 1)
#define HUB_PORT_RESETTING (1U << 4)
#define HUB_PORT_HIGH_SPEED (1U << 10)
#define HUB_MAX_PORTS 15U
#define HUB_RESET_POLL_NS (10ULL * 1000ULL * 1000ULL)
#define HUB_RESET_ATTEMPTS 25U

static int hub_port_status(struct ehci *host, struct ehci_device *hub,
                           unsigned port, uint32_t *out) {
    uint8_t status[4];
    if (control_transfer(host, hub, 0xA3U, HUB_REQUEST_GET_STATUS, 0,
                         (uint16_t)port, 4, status) != 0) return -1;
    *out = (uint32_t)status[0] | ((uint32_t)status[1] << 8);
    return 0;
}

static int hub_port_feature(struct ehci *host, struct ehci_device *hub,
                            unsigned port, uint16_t feature, int set) {
    return control_transfer(host, hub, 0x23U,
                            set ? HUB_REQUEST_SET_FEATURE
                                : HUB_REQUEST_CLEAR_FEATURE,
                            feature, (uint16_t)port, 0, NULL);
}

/*
 * Reset one hub port and report whether a high-speed device came up on it.
 *
 * The sequence is the one the root ports go through, spoken over the control
 * pipe instead of written to a register: ask the hub to reset the port, watch
 * its status until the reset clears, and read the speed out of the answer.
 */
static int hub_reset_port(struct ehci *host, struct ehci_device *hub,
                          unsigned port, unsigned where) {
    if (hub_port_feature(host, hub, port, HUB_FEATURE_PORT_RESET, 1) != 0)
        return -1;

    uint32_t status = 0;
    for (unsigned attempt = 0; attempt < HUB_RESET_ATTEMPTS; attempt++) {
        delay_ns(HUB_RESET_POLL_NS);
        if (hub_port_status(host, hub, port, &status) != 0) return -1;
        if (!(status & HUB_PORT_RESETTING)) break;
    }
    if (status & HUB_PORT_RESETTING) return -1;
    hub_port_feature(host, hub, port, HUB_FEATURE_C_PORT_RESET, 0);

    if (!(status & HUB_PORT_ENABLED)) return -1;
    if (!(status & HUB_PORT_HIGH_SPEED)) {
        /* Reachable only through split transactions, which this driver does
           not do. Saying so is better than leaving a live port silent. */
        kprintf("EHCI: port %s is not high speed, skipped\n", port_name(where));
        return -1;
    }
    delay_ns(RESET_RECOVERY_NS);
    return 0;
}

static void enumerate_hub(struct ehci *host, struct ehci_device *hub,
                          unsigned root_port, uint8_t *next_address) {
    /*
     * Configured first, and this is not a formality: a device in the address
     * state is not required to answer anything but the standard requests, and
     * everything below is a class request to one of its ports.
     */
    uint8_t configuration[9];
    if (control_transfer(host, hub, 0x80U, USB_REQUEST_GET_DESCRIPTOR,
                         (uint16_t)(USB_DESCRIPTOR_CONFIGURATION << 8), 0, 9,
                         configuration) != 0 ||
        control_transfer(host, hub, 0x00U, USB_REQUEST_SET_CONFIGURATION,
                         configuration[5], 0, 0, NULL) != 0) {
        kprintf("EHCI: the hub on port %u would not configure\n", root_port);
        return;
    }

    uint8_t descriptor[8];
    if (control_transfer(host, hub, 0xA0U, USB_REQUEST_GET_DESCRIPTOR,
                         (uint16_t)(HUB_DESCRIPTOR_TYPE << 8), 0, 8,
                         descriptor) != 0) {
        kprintf("EHCI: the hub on port %u has no descriptor\n", root_port);
        return;
    }
    unsigned ports = descriptor[2];
    if (ports > HUB_MAX_PORTS) ports = HUB_MAX_PORTS;
    /* bPwrOn2PwrGood is in units of two milliseconds, and is the hub saying
       how long its ports take to come up after being told to. */
    uint64_t power_good_ns = (uint64_t)descriptor[5] * 2ULL * 1000ULL * 1000ULL;
    kprintf("EHCI: port %u is a hub with %u ports\n", root_port, ports);

    for (unsigned port = 1; port <= ports; port++)
        hub_port_feature(host, hub, port, HUB_FEATURE_PORT_POWER, 1);
    delay_ns(power_good_ns + PORT_POWER_SETTLE_NS);

    for (unsigned port = 1; port <= ports; port++) {
        if (host->device_count >= MAX_DEVICES) return;
        unsigned where = (root_port << 4) | port;
        uint32_t status = 0;
        if (hub_port_status(host, hub, port, &status) != 0) continue;
        if (!(status & HUB_PORT_CONNECTED)) continue;
        if (hub_reset_port(host, hub, port, where) != 0) continue;

        /* On the stack, not in the table: only a disk earns a slot, and the
           hub is still using its own entry to answer these requests. */
        struct ehci_device candidate;
        int class_code = address_device(host, &candidate, *next_address, where);
        if (class_code < 0) continue;
        (*next_address)++;
        /* One level. A hub behind a hub is not something a chipset does to
           itself, and following it would need a queue this does not have. */
        if (class_code == (int)USB_CLASS_HUB) {
            kprintf("EHCI: port %s is a second hub, not followed\n", port_name(where));
            continue;
        }
        if (enumerate_storage(host, &candidate, where) == 0)
            host->devices[host->device_count++] = candidate;
    }
}

static void enumerate_ports(struct ehci *host) {
    /* Addresses are numbered per controller because each controller is its
       own bus, with its own address space for the devices on it. */
    uint8_t next_address = 1;
    for (unsigned port = 0; port < host->ports && port < MAX_PORTS; port++) {
        if (host->device_count >= MAX_DEVICES) break;
        if (reset_port(host, port) != 0) continue;

        unsigned where = (port + 1U) << 4;
        struct ehci_device candidate;
        int class_code = address_device(host, &candidate, next_address, where);
        if (class_code < 0) continue;
        next_address++;

        if (class_code == (int)USB_CLASS_HUB) {
            enumerate_hub(host, &candidate, port + 1U, &next_address);
            continue;
        }
        if (enumerate_storage(host, &candidate, where) == 0)
            host->devices[host->device_count++] = candidate;
    }
    if (host->device_count) return;

    /* Nothing came up, and this is where the interesting information is: a
       port with nothing in it, a port handed to the companion and a port that
       refused to enable all look the same from outside and are three different
       problems. PORTSC tells them apart, so on a machine with nothing on its
       serial port, print it. */
    for (unsigned port = 0; port < host->ports && port < MAX_PORTS; port++)
        kprintf("EHCI: port %u idle, status %x\n", port + 1U,
                (unsigned)mmio_read32(port_register(host, port)));
}

/* --- what the mass-storage transport above needs -------------------------- */

static struct ehci_device *storage_device(int index, struct ehci **host_out) {
    int seen = 0;
    for (unsigned which = 0; which < controller_count; which++) {
        struct ehci *host = &controllers[which];
        for (unsigned at = 0; at < MAX_DEVICES; at++) {
            if (!host->devices[at].used || !host->devices[at].is_storage) continue;
            if (seen == index) {
                *host_out = host;
                return &host->devices[at];
            }
            seen++;
        }
    }
    return NULL;
}

static int ehci_storage_count(void) {
    int count = 0;
    for (unsigned which = 0; which < controller_count; which++)
        for (unsigned at = 0; at < MAX_DEVICES; at++)
            if (controllers[which].devices[at].used &&
                controllers[which].devices[at].is_storage) count++;
    return count;
}

/* Clear a halted endpoint. A halt stays until it is cleared and every transfer
   after it waits out the timeout first, so the machine crawls rather than
   stops. The device resets its data toggle with the halt, which is why ours
   goes with it. */
static int clear_endpoint_halt(struct ehci *host, struct ehci_device *device,
                               uint8_t endpoint, int in) {
    uint16_t address = (uint16_t)(endpoint | (in ? 0x80U : 0x00U));
    return control_transfer(host, device, 0x02U, USB_REQUEST_CLEAR_FEATURE,
                            USB_FEATURE_ENDPOINT_HALT, address, 0, NULL);
}

static int ehci_bulk_transfer(int index, int in, uint64_t physical,
                              uint32_t length) {
    struct ehci *host = NULL;
    struct ehci_device *device = storage_device(index, &host);
    if (!device) return -1;
    if (physical + length > DMA_LIMIT) {
        kprintf("EHCI: a buffer above 4 GiB cannot be described\n");
        return -1;
    }

    uint8_t endpoint = in ? device->bulk_in_endpoint : device->bulk_out_endpoint;
    uint16_t packet = in ? device->bulk_in_packet : device->bulk_out_packet;
    uint8_t *toggle = in ? &device->bulk_in_toggle : &device->bulk_out_toggle;

    build_qtd(&qtds[0], in ? QTD_PID_IN : QTD_PID_OUT, physical, length, *toggle);
    int failed = run_qtds(host, device, endpoint, packet, 0, &qtds[0], &qtds[0],
                          TRANSFER_TIMEOUT_NS) != 0;
    uint32_t token = dma_load32(&qtds[0].token);

    if (failed) {
        /* The controller works on a copy of the descriptor in the queue head's
           overlay and only writes that back when the descriptor retires, so
           the overlay is where its own state is. */
        uint32_t overlay = dma_load32(&host->work_qh->overlay_token);
        uint32_t current = dma_load32(&host->work_qh->current_qtd);

        /* Worth saying out loud, and worth saying only a few times: a disk
           that has started failing fails on every block after it, and the
           first few lines are the ones that name what went wrong. */
        /* The first few, and then one in every so many: a run that fails for
           minutes used to say nothing at all after the eighth line, which is
           exactly the run whose later failures are worth seeing. */
        static unsigned seen;
        seen++;
        if (seen <= BULK_FAILURES_REPORTED || seen % BULK_FAILURE_INTERVAL == 0) {
            /* USBSTS says whether the asynchronous schedule was running at all. */
            uint32_t status = mmio_read32(operational(host, EHCI_USBSTS));
            uint32_t command = mmio_read32(operational(host, EHCI_USBCMD));
            kprintf("EHCI: bulk %s endpoint %u failed (%u so far), token %x "
                    "overlay %x current %x usbsts %x usbcmd %x\n",
                    in ? "in" : "out", (unsigned)endpoint, seen, (unsigned)token,
                    (unsigned)overlay, (unsigned)current,
                    (unsigned)status, (unsigned)command);
        }
        /* The toggle is reset only when the halt is cleared, because that is
           the only thing that resets the device's -- a transfer that merely
           timed out moved nothing at either end. The overlay counts as much as
           the descriptor: a stalled endpoint halts the queue head, and a
           halted queue head never retires the descriptor the halt would have
           been written back to. */
        if ((token | overlay) & QTD_STATUS_HALTED) {
            if (clear_endpoint_halt(host, device, endpoint, in) == 0) {
                *toggle = 0;
            } else if (seen <= BULK_FAILURES_REPORTED) {
                /* Worth its own line: a halt that will not clear is a device
                   that needs the class reset, not another transfer. */
                kprintf("EHCI: the halt on endpoint %u would not clear\n",
                        (unsigned)endpoint);
            }
        }
        return -1;
    }

    /* One toggle per packet actually moved, which is not the same as one per
       packet asked for: a device is allowed to end a transfer early, and the
       bytes it did not send are counted in the descriptor it hands back. */
    uint32_t remaining = (token >> QTD_LENGTH_SHIFT) & 0x7FFFU;
    uint32_t moved = length > remaining ? length - remaining : 0;
    uint32_t packets = packet ? (moved + packet - 1U) / packet : 0;
    *toggle = (uint8_t)((*toggle + packets) & 1U);
    return 0;
}

/* Bulk-only mass storage error recovery, as the class specification defines
   it: a class request to the interface, then the halt cleared on both bulk
   endpoints, whose toggles reset with it. Without it the device reads the next
   command wrapper as the data it was still expecting. */
#define BULK_ONLY_RESET 0xFFU

static int ehci_reset_recovery(int index) {
    struct ehci *host = NULL;
    struct ehci_device *device = storage_device(index, &host);
    if (!device) return -1;

    if (control_transfer(host, device, 0x21U, BULK_ONLY_RESET, 0,
                         device->interface, 0, NULL) != 0) return -1;
    (void)clear_endpoint_halt(host, device, device->bulk_in_endpoint, 1);
    (void)clear_endpoint_halt(host, device, device->bulk_out_endpoint, 0);
    device->bulk_in_toggle = 0;
    device->bulk_out_toggle = 0;
    return 0;
}

static const struct usb_host ehci_host = {
    .name = "ehci",
    .storage_count = ehci_storage_count,
    .bulk_transfer = ehci_bulk_transfer,
    .reset_recovery = ehci_reset_recovery,
};

/* Bring one controller up. Failing here is not fatal to the others. */
static int start_one(const struct pci_device *device, struct ehci *host) {
    if (device->bar[0] & PCI_BAR_IO) return -1;
    uint64_t physical = device->bar[0] & PCI_BAR_ADDRESS_MASK;
    if ((device->bar[0] & PCI_BAR_TYPE_MASK) == PCI_BAR_TYPE_64BIT)
        physical |= (uint64_t)device->bar[1] << 32;
    if (!physical) return -1;

    host->base = vmm_map_device(physical, EHCI_REGISTER_BYTES);
    if (!host->base) {
        kprintf("EHCI: could not map registers at %x\n", (unsigned)physical);
        return -1;
    }
    pci_enable_bus_mastering(device);

    uint32_t length_and_version = mmio_read32(host->base + EHCI_CAPLENGTH);
    uint32_t structural = mmio_read32(host->base + EHCI_HCSPARAMS);
    uint32_t capabilities = mmio_read32(host->base + EHCI_HCCPARAMS);

    host->version = (uint16_t)(length_and_version >> 16);
    host->operational = host->base + (uint8_t)length_and_version;
    host->ports = structural & HCSPARAMS_PORTS_MASK;
    if (!host->ports) {
        kprintf("EHCI: controller at %x reports no ports\n", (unsigned)physical);
        return -1;
    }

    release_from_firmware(device, capabilities);
    if (reset_controller(host) != 0) return -1;

    build_async_ring(host);
    if (start_controller(host) != 0) return -1;

    host->present = 1;
    kprintf("EHCI: %x.%x at %x, %u ports, async schedule running\n",
            (unsigned)(host->version >> 8), (unsigned)(host->version & 0xFF),
            (unsigned)physical, (unsigned)host->ports);
    return 0;
}

int ehci_init(void) {
    /* Every USB controller the machine has is listed, whether this driver can
       use it or not. On hardware with no serial port this log is the only way
       to find out why a stick was not seen, and "there is no EHCI here" and
       "the EHCI here found nothing" are very different answers. */
    struct pci_device device;
    for (unsigned nth = 0;
         pci_find_nth_class(PCI_CLASS_SERIAL_BUS, PCI_SUBCLASS_USB, nth,
                            &device) == 0; nth++) {
        const char *kind = "unknown";
        if (device.prog_if == 0x00U) kind = "uhci";
        else if (device.prog_if == 0x10U) kind = "ohci";
        else if (device.prog_if == PCI_PROG_IF_EHCI) kind = "ehci";
        else if (device.prog_if == 0x30U) kind = "xhci";
        kprintf("USB: %s at %x:%x.%x\n", kind, (unsigned)device.bus,
                (unsigned)device.slot, (unsigned)device.function);
    }

    /* The descriptors and buffers, once, for however many controllers there
       turn out to be: they are shared, and a transfer uses them one at a
       time. The ring heads are per controller and come out of the same page. */
    dma_page = (uint8_t *)dma_alloc_page(&dma_physical);
    if (!dma_page) {
        kprintf("EHCI: no DMA memory below 4 GiB\n");
        return -1;
    }
    /* Two things decide the offsets: the controller wants 32-byte alignment,
       and a queue head is 84 bytes rather than the 64 it looks like -- the
       overlay is most of it. A whole 256 bytes apart leaves no room to get
       that wrong, and the first version of this driver did. */
    for (unsigned which = 0; which < MAX_CONTROLLERS; which++) {
        controllers[which].async_head =
            (struct ehci_qh *)(dma_page + which * 0x100);
        controllers[which].work_qh =
            (struct ehci_qh *)(dma_page + 0x400 + which * 0x100);
    }
    qtds = (struct ehci_qtd *)(dma_page + 0x800);
    setup_buffer = dma_page + 0x900;
    descriptor_buffer = dma_page + 0xA00;

    for (unsigned nth = 0; controller_count < MAX_CONTROLLERS; nth++) {
        if (pci_find_nth_class(PCI_CLASS_SERIAL_BUS, PCI_SUBCLASS_USB, nth,
                               &device) != 0) break;
        if (device.prog_if != PCI_PROG_IF_EHCI) continue;
        if (start_one(&device, &controllers[controller_count]) == 0) controller_count++;
        else memset(&controllers[controller_count], 0, sizeof(struct ehci));
    }

    if (!controller_count) return -1;

    /* One debounce for every controller rather than one each: they were all
       powered above and the interval is the same interval. */
    delay_ns(PORT_POWER_SETTLE_NS);
    for (unsigned which = 0; which < controller_count; which++)
        enumerate_ports(&controllers[which]);

    if (ehci_storage_count()) usb_register_host(&ehci_host);
    return 0;
}
