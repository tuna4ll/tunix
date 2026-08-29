/*
 * EHCI: the USB 2.0 host controller, and on a machine old enough to boot with
 * a BIOS the only one there is.
 *
 * It exists for one reason. The bootloader reads the kernel off a USB stick
 * through the firmware, hands over, and from that moment the stick is only
 * reachable by whatever driver the kernel has -- so on a pre-xHCI machine the
 * root filesystem was on a disk nothing here could see, and the boot ended in
 * `root filesystem mount failed` with the internal disk listed and the stick
 * absent.
 *
 * What this driver does and does not do is worth stating plainly, because EHCI
 * is a controller with a companion:
 *
 * - High-speed devices only. A full- or low-speed device on the same port is
 *   handed to the companion UHCI/OHCI controller by writing Port Owner, which
 *   is what the specification asks for and what the firmware expects. There is
 *   no companion driver here, so such a device is simply not reached -- and it
 *   does not matter, because a USB stick is high speed and a keyboard behind
 *   the BIOS's legacy emulation never gets this far.
 * - No split transactions and no hubs. A device behind a hub is not found.
 * - Mass storage only, no HID. See above: the devices EHCI would have to talk
 *   to at full speed are the ones this driver deliberately gives away.
 *
 * The schedule is the asynchronous one and nothing else. A queue head sits in
 * a ring that points at itself, transfer descriptors are appended to it, and
 * the controller walks the ring on its own -- there is no doorbell in EHCI,
 * which is why every transfer here ends in a poll on the descriptor's own
 * status byte rather than on an event ring.
 */
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
#define USBCMD_INTERRUPT_THRESHOLD_SHIFT 16U

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
#define USB_DESCRIPTOR_DEVICE 0x01U
#define USB_DESCRIPTOR_CONFIGURATION 0x02U

#define USB_CLASS_MASS_STORAGE 0x08U
#define USB_SUBCLASS_SCSI 0x06U
#define USB_PROTOCOL_BULK_ONLY 0x50U

#define RESET_TIMEOUT_NS (1000ULL * 1000ULL * 1000ULL)
#define PORT_RESET_HOLD_NS (50ULL * 1000ULL * 1000ULL)
#define PORT_ENABLE_TIMEOUT_NS (200ULL * 1000ULL * 1000ULL)
#define TRANSFER_TIMEOUT_NS (2000ULL * 1000ULL * 1000ULL)
#define HANDOFF_TIMEOUT_NS (1000ULL * 1000ULL * 1000ULL)
/* The specification's recovery time after a port reset, before the device is
   required to answer on its default address. */
#define RESET_RECOVERY_NS (20ULL * 1000ULL * 1000ULL)
#define SET_ADDRESS_RECOVERY_NS (10ULL * 1000ULL * 1000ULL)

#define EHCI_REGISTER_BYTES 0x1000U
#define MAX_PORTS 15U
#define MAX_DEVICES 4U
/* Four ring heads fit in the front of the DMA page, and no chipset has more
   than the two an old Intel one splits its ports across. */
#define MAX_CONTROLLERS 4U
#define CONFIGURATION_BYTES 512U

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

/*
 * One of these per controller. An old Intel chipset splits its ports across
 * two EHCI controllers, and the stick is on whichever half the port belongs
 * to, so finding one and stopping is the same as not looking.
 *
 * Only the ring head is per controller. The working queue head, the transfer
 * descriptors and the buffers are shared, because a transfer here is
 * synchronous: exactly one of them is linked into exactly one ring at a time.
 */
struct ehci {
    int present;
    uint16_t version;
    uint64_t base;
    uint64_t operational;
    unsigned ports;
    struct ehci_qh *async_head;
    struct ehci_device devices[MAX_DEVICES];
    unsigned device_count;
};

static struct ehci controllers[MAX_CONTROLLERS];
static unsigned controller_count;

/* One page holds every structure the controllers read. */
static uint8_t *dma_page;
static uint64_t dma_physical;
static struct ehci_qh *work_qh;
static struct ehci_qtd *qtds;
static uint8_t *setup_buffer;
static uint8_t *descriptor_buffer;

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

/*
 * One page of DMA memory below 4 GiB.
 *
 * Every pointer the controller follows is a 32-bit value whose high half comes
 * from CTRLDSSEGMENT and is shared by all of them, so a structure above 4 GiB
 * is not something this driver can describe. Rather than program a segment it
 * could not then honour for a buffer handed down from the transport above,
 * everything stays in the low 4 GiB and CTRLDSSEGMENT stays zero.
 */
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

/*
 * Take the controller from the firmware.
 *
 * Until this handshake completes the BIOS still owns the controller and still
 * services its interrupts, and everything written to the operational registers
 * is liable to be undone. The capability is optional: if HCCPARAMS names no
 * pointer there is nothing to take.
 */
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
    memset(async_head, 0, sizeof(*async_head));
    async_head->horizontal = physical_of(async_head) | LINK_TYPE_QH;
    async_head->characteristics = QH_HEAD_OF_LIST | QH_SPEED_HIGH |
                                  (64U << QH_MAX_PACKET_SHIFT);
    async_head->capabilities = (1U << QH_MULT_SHIFT);
    async_head->overlay_next = LINK_TERMINATE;
    async_head->overlay_alternate = LINK_TERMINATE;
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
    delay_ns(RESET_RECOVERY_NS);
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

    if (!(status & PORTSC_POWER)) {
        mmio_write32(port_register(host, port),
                     (status & ~PORTSC_CHANGE_BITS) | PORTSC_POWER);
        delay_ns(PORT_RESET_HOLD_NS);
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
    memset(qtd, 0, sizeof(*qtd));
    qtd->next = LINK_TERMINATE;
    qtd->alternate = LINK_TERMINATE;
    qtd->token = (length << QTD_LENGTH_SHIFT) | (3U << QTD_ERROR_COUNT_SHIFT) |
                 (pid << QTD_PID_SHIFT) | QTD_STATUS_ACTIVE |
                 QTD_INTERRUPT_ON_COMPLETE;
    if (toggle) qtd->token |= QTD_DATA_TOGGLE;

    if (!length) return;
    uint64_t address = physical;
    uint64_t end = physical + length;
    for (unsigned page = 0; page < 5U && address < end; page++) {
        qtd->buffer[page] = (uint32_t)address;
        address = (address & ~0xFFFULL) + 0x1000ULL;
    }
}

/*
 * Point the working queue head at a chain of descriptors, link it into the
 * ring, and wait for the last one to go inactive.
 *
 * The queue head is rewritten rather than reused because every transfer here
 * is synchronous: there is never a second one in flight to disturb. It is
 * unlinked again at the end for the same reason -- leaving it in the ring
 * would have the controller walking descriptors that are about to change.
 */
static int run_qtds(struct ehci *host, struct ehci_device *device,
                    uint8_t endpoint, uint16_t max_packet, int is_control,
                    struct ehci_qtd *first, struct ehci_qtd *last) {
    struct ehci_qh *async_head = host->async_head;
    memset(work_qh, 0, sizeof(*work_qh));
    work_qh->characteristics = device->address |
                               ((uint32_t)endpoint << QH_ENDPOINT_SHIFT) |
                               QH_SPEED_HIGH | QH_DATA_TOGGLE_CONTROL |
                               ((uint32_t)max_packet << QH_MAX_PACKET_SHIFT) |
                               (3U << QH_RELOAD_SHIFT);
    /* The control-endpoint flag is for full- and low-speed endpoints only.
       Setting it on a high-speed one, which is all this driver talks to, makes
       the controller run a protocol the device is not speaking. */
    (void)is_control;
    work_qh->capabilities = (1U << QH_MULT_SHIFT);
    work_qh->current_qtd = 0;
    work_qh->overlay_next = physical_of(first);
    work_qh->overlay_alternate = LINK_TERMINATE;
    work_qh->overlay_token = 0;
    work_qh->horizontal = physical_of(async_head) | LINK_TYPE_QH;

    async_head->horizontal = physical_of(work_qh) | LINK_TYPE_QH;

    uint64_t deadline = time_uptime_ns() + TRANSFER_TIMEOUT_NS;
    int status = -1;
    for (;;) {
        uint32_t token = *(volatile uint32_t *)&last->token;
        if (!(token & QTD_STATUS_ACTIVE)) {
            status = (token & QTD_STATUS_ERROR_MASK) ? -1 : 0;
            break;
        }
        if (*(volatile uint32_t *)&work_qh->overlay_token & QTD_STATUS_HALTED) break;
        if (time_uptime_ns() >= deadline) break;
        __asm__ volatile("pause");
    }

    async_head->horizontal = physical_of(async_head) | LINK_TYPE_QH;
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
                          status_qtd);
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

static int enumerate_device(struct ehci *host, unsigned port, uint8_t address) {
    if (host->device_count >= MAX_DEVICES) return -1;
    struct ehci_device *device = &host->devices[host->device_count];
    memset(device, 0, sizeof(*device));
    /* Address zero and the smallest packet size the specification allows,
       until the device has said otherwise: the first eight bytes of its
       descriptor are the ones that name the real size. */
    device->address = 0;
    device->max_packet = 64;

    uint8_t header[8];
    if (control_transfer(host, device, 0x80U, USB_REQUEST_GET_DESCRIPTOR,
                         (uint16_t)(USB_DESCRIPTOR_DEVICE << 8), 0, 8,
                         header) != 0) {
        kprintf("EHCI: port %u did not answer GET_DESCRIPTOR\n", port + 1U);
        return -1;
    }
    if (header[7]) device->max_packet = header[7];

    if (control_transfer(host, device, 0x00U, USB_REQUEST_SET_ADDRESS, address, 0, 0,
                         NULL) != 0) {
        kprintf("EHCI: port %u refused SET_ADDRESS\n", port + 1U);
        return -1;
    }
    device->address = address;
    delay_ns(SET_ADDRESS_RECOVERY_NS);

    uint8_t header9[9];
    if (control_transfer(host, device, 0x80U, USB_REQUEST_GET_DESCRIPTOR,
                         (uint16_t)(USB_DESCRIPTOR_CONFIGURATION << 8), 0, 9,
                         header9) != 0)
        return -1;
    uint16_t total = (uint16_t)(header9[2] | ((uint16_t)header9[3] << 8));
    if (total > CONFIGURATION_BYTES) total = CONFIGURATION_BYTES;

    static uint8_t configuration[CONFIGURATION_BYTES];
    if (control_transfer(host, device, 0x80U, USB_REQUEST_GET_DESCRIPTOR,
                         (uint16_t)(USB_DESCRIPTOR_CONFIGURATION << 8), 0, total,
                         configuration) != 0)
        return -1;

    if (find_storage_interface(device, configuration, total) != 0) return -1;
    device->configuration = header9[5];

    if (control_transfer(host, device, 0x00U, USB_REQUEST_SET_CONFIGURATION,
                         device->configuration, 0, 0, NULL) != 0) {
        kprintf("EHCI: port %u refused SET_CONFIGURATION\n", port + 1U);
        return -1;
    }

    device->used = 1;
    device->is_storage = 1;
    host->device_count++;
    kprintf("EHCI: port %u: mass storage at address %u, bulk in %u out %u\n",
            port + 1U, (unsigned)address, (unsigned)device->bulk_in_endpoint,
            (unsigned)device->bulk_out_endpoint);
    return 0;
}

static void enumerate_ports(struct ehci *host) {
    /* Addresses are numbered per controller because each controller is its
       own bus, with its own address space for the devices on it. */
    uint8_t next_address = 1;
    for (unsigned port = 0; port < host->ports && port < MAX_PORTS; port++) {
        if (reset_port(host, port) != 0) continue;
        if (enumerate_device(host, port, next_address) == 0) next_address++;
    }
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
    if (run_qtds(host, device, endpoint, packet, 0, &qtds[0], &qtds[0]) != 0) {
        /* A halted endpoint leaves its toggle where the failure put it, and
           the transport above answers a failure by starting the command over.
           Clearing it here is what keeps the retry from being rejected. */
        *toggle = 0;
        return -1;
    }

    /* One toggle per packet, and a bulk transfer is a whole number of them. */
    uint32_t packets = packet ? (length + packet - 1U) / packet : 0;
    *toggle = (uint8_t)((*toggle + packets) & 1U);
    return 0;
}

static const struct usb_host ehci_host = {
    .name = "ehci",
    .storage_count = ehci_storage_count,
    .bulk_transfer = ehci_bulk_transfer,
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
    enumerate_ports(host);
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
    for (unsigned which = 0; which < MAX_CONTROLLERS; which++)
        controllers[which].async_head =
            (struct ehci_qh *)(dma_page + which * 0x100);
    work_qh = (struct ehci_qh *)(dma_page + 0x400);
    qtds = (struct ehci_qtd *)(dma_page + 0x500);
    setup_buffer = dma_page + 0x600;
    descriptor_buffer = dma_page + 0x700;

    for (unsigned nth = 0; controller_count < MAX_CONTROLLERS; nth++) {
        if (pci_find_nth_class(PCI_CLASS_SERIAL_BUS, PCI_SUBCLASS_USB, nth,
                               &device) != 0) break;
        if (device.prog_if != PCI_PROG_IF_EHCI) continue;
        if (start_one(&device, &controllers[controller_count]) == 0) controller_count++;
        else memset(&controllers[controller_count], 0, sizeof(struct ehci));
    }

    if (!controller_count) return -1;
    if (ehci_storage_count()) usb_register_host(&ehci_host);
    return 0;
}
