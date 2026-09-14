#include <stdint.h>
#include <stddef.h>

#include "../../include/cpu.h"
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

#define EHCI_CAPLENGTH 0x00U
#define EHCI_HCSPARAMS 0x04U
#define EHCI_HCCPARAMS 0x08U

#define HCSPARAMS_PORTS_MASK 0x0FU
#define HCCPARAMS_EECP_SHIFT 8U
#define HCCPARAMS_EECP_MASK 0xFFU

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
#define PORTSC_CHANGE_BITS (PORTSC_CONNECT_CHANGE | PORTSC_ENABLE_CHANGE | PORTSC_OVERCURRENT_CHANGE)

#define EHCI_LEGACY_CAPABILITY_ID 0x01U
#define LEGACY_BIOS_OWNED (1U << 16)
#define LEGACY_OS_OWNED (1U << 24)

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
#define TRANSFER_TIMEOUT_NS (10000ULL * 1000ULL * 1000ULL)
#define TRANSFER_QUIET_NS (2000ULL * 1000ULL * 1000ULL)
#define CONTROL_TIMEOUT_NS (300ULL * 1000ULL * 1000ULL)
#define HANDOFF_TIMEOUT_NS (1000ULL * 1000ULL * 1000ULL)
#define RESET_RECOVERY_NS (20ULL * 1000ULL * 1000ULL)
#define PORT_POWER_SETTLE_NS (100ULL * 1000ULL * 1000ULL)
#define SET_ADDRESS_RECOVERY_NS (10ULL * 1000ULL * 1000ULL)

#define EHCI_REGISTER_BYTES 0x1000U
#define MAX_PORTS 15U
#define MAX_DEVICES 8U
#define MAX_CONTROLLERS 4U
#define CONFIGURATION_BYTES 512U
#define BULK_FAILURES_REPORTED 8U
#define BULK_FAILURE_INTERVAL 64U

struct ehci_qtd {
    uint32_t next;
    uint32_t alternate;
    uint32_t token;
    uint32_t buffer[5];
    uint32_t buffer_high[5];
    uint32_t reserved[3];
};

struct ehci_qh {
    uint32_t horizontal;
    uint32_t characteristics;
    uint32_t capabilities;
    uint32_t current_qtd;
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
    uint8_t bulk_in_toggle;
    uint8_t bulk_out_toggle;
};

typedef char ehci_qh_size_check[(sizeof(struct ehci_qh) <= 0x100) ? 1 : -1];
typedef char ehci_qtd_size_check[(3 * sizeof(struct ehci_qtd) <= 0x100) ? 1 : -1];

struct ehci {
    int present;
    uint16_t version;
    uint64_t base;
    uint64_t operational;
    unsigned ports;
    struct ehci_qh *async_head;
    struct ehci_qh *work_qh;
    uint32_t work_characteristics;
    uint32_t work_capabilities;
    struct ehci_device devices[MAX_DEVICES];
    unsigned device_count;
};

static struct ehci controllers[MAX_CONTROLLERS];
static unsigned controller_count;

static uint8_t *dma_page;
static uint64_t dma_physical;
static struct ehci_qtd *qtds;
static uint8_t *setup_buffer;
static uint8_t *descriptor_buffer;

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

static int wait_for(uint64_t address, uint32_t mask, uint32_t wanted,
                    uint64_t timeout_ns) {
    uint64_t deadline = time_uptime_ns() + timeout_ns;
    for (;;) {
        if ((mmio_read32(address) & mask) == wanted) return 0;
        if (time_uptime_ns() >= deadline) return -1;
        cpu_relax();
    }
}

static void delay_ns(uint64_t nanoseconds) {
    uint64_t deadline = time_uptime_ns() + nanoseconds;
    while (time_uptime_ns() < deadline) cpu_relax();
}

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
        cpu_relax();
    }
    kprintf("EHCI: firmware did not release the controller, taking it\n");
    pci_config_write32(device->bus, device->slot, device->function, pointer,
                       (legacy & ~LEGACY_BIOS_OWNED) | LEGACY_OS_OWNED);
}

static int reset_controller(struct ehci *host) {
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

static void build_async_ring(struct ehci *host) {
    struct ehci_qh *async_head = host->async_head;
    struct ehci_qh *work = host->work_qh;

    memset(async_head, 0, sizeof(*async_head));
    memset(work, 0, sizeof(*work));

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
    if (wait_for(operational(host, EHCI_USBSTS), USBSTS_ASYNC_RUNNING,
                 USBSTS_ASYNC_RUNNING, RESET_TIMEOUT_NS) != 0) {
        kprintf("EHCI: the asynchronous schedule would not start\n");
        return -1;
    }
    mmio_write32(operational(host, EHCI_CONFIGFLAG), 1U);

    for (unsigned port = 0; port < host->ports && port < MAX_PORTS; port++) {
        uint32_t status = mmio_read32(port_register(host, port));
        mmio_write32(port_register(host, port),
                     (status & ~(PORTSC_CHANGE_BITS | PORTSC_OWNER)) | PORTSC_POWER);
    }
    return 0;
}

static void release_port(struct ehci *host, unsigned port) {
    uint32_t status = mmio_read32(port_register(host, port));
    mmio_write32(port_register(host, port),
                 (status & ~PORTSC_CHANGE_BITS) | PORTSC_OWNER);
}

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
    dma_store32(&qtd->token, token);
}

#define ASYNC_KICK_AFTER_NS (20ULL * 1000ULL * 1000ULL)
#define ASYNC_KICK_TIMEOUT_NS (100ULL * 1000ULL * 1000ULL)

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

static int run_qtds(struct ehci *host, struct ehci_device *device,
                    uint8_t endpoint, uint16_t max_packet, int is_control,
                    struct ehci_qtd *first, struct ehci_qtd *last,
                    uint64_t timeout_ns) {
    struct ehci_qh *work_qh = host->work_qh;

    dma_store32(&work_qh->overlay_next, LINK_TERMINATE);
    dma_store32(&work_qh->overlay_token, 0);
    dma_store32(&work_qh->overlay_alternate, LINK_TERMINATE);
    dma_store32(&work_qh->current_qtd, 0);

    (void)is_control;
    uint32_t characteristics = device->address |
                               ((uint32_t)endpoint << QH_ENDPOINT_SHIFT) |
                               QH_SPEED_HIGH | QH_DATA_TOGGLE_CONTROL |
                               ((uint32_t)max_packet << QH_MAX_PACKET_SHIFT) |
                               (3U << QH_RELOAD_SHIFT);
    uint32_t capabilities = (1U << QH_MULT_SHIFT);
    if (characteristics != host->work_characteristics ||
        capabilities != host->work_capabilities) {
        dma_store32(&work_qh->characteristics, characteristics);
        dma_store32(&work_qh->capabilities, capabilities);
        host->work_characteristics = characteristics;
        host->work_capabilities = capabilities;
    }

    dma_store32(&work_qh->overlay_next, physical_of(first));

    uint64_t started = time_uptime_ns();
    uint64_t deadline = started + timeout_ns;
    uint64_t kick_at = started + ASYNC_KICK_AFTER_NS;
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
        if (!kicked && now >= kick_at) {
            kicked = 1;
            async_kick(host);
        }
        cpu_relax();
    }

    dma_store32(&work_qh->overlay_next, LINK_TERMINATE);
    if (abandoned) async_advance(host);
    return status;
}

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
    build_qtd(status_qtd, in ? QTD_PID_OUT : QTD_PID_IN, 0, 0, 1);

    if (length && !in && data) memcpy(descriptor_buffer, data, length);
    int result = run_qtds(host, device, 0, device->max_packet, 1, setup_qtd,
                          status_qtd, CONTROL_TIMEOUT_NS);
    if (result == 0 && length && in && data) memcpy(data, descriptor_buffer, length);
    return result;
}

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

static int address_device(struct ehci *host, struct ehci_device *device,
                          uint8_t address, unsigned where) {
    memset(device, 0, sizeof(*device));
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
        kprintf("EHCI: port %s is not high speed, skipped\n", port_name(where));
        return -1;
    }
    delay_ns(RESET_RECOVERY_NS);
    return 0;
}

static void enumerate_hub(struct ehci *host, struct ehci_device *hub,
                          unsigned root_port, uint8_t *next_address) {
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

        struct ehci_device candidate;
        int class_code = address_device(host, &candidate, *next_address, where);
        if (class_code < 0) continue;
        (*next_address)++;
        if (class_code == (int)USB_CLASS_HUB) {
            kprintf("EHCI: port %s is a second hub, not followed\n", port_name(where));
            continue;
        }
        if (enumerate_storage(host, &candidate, where) == 0)
            host->devices[host->device_count++] = candidate;
    }
}

static void enumerate_ports(struct ehci *host) {
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

    for (unsigned port = 0; port < host->ports && port < MAX_PORTS; port++)
        kprintf("EHCI: port %u idle, status %x\n", port + 1U,
                (unsigned)mmio_read32(port_register(host, port)));
}

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
        uint32_t overlay = dma_load32(&host->work_qh->overlay_token);
        uint32_t current = dma_load32(&host->work_qh->current_qtd);

        static unsigned seen;
        seen++;
        if (seen <= BULK_FAILURES_REPORTED || seen % BULK_FAILURE_INTERVAL == 0) {
            uint32_t status = mmio_read32(operational(host, EHCI_USBSTS));
            uint32_t command = mmio_read32(operational(host, EHCI_USBCMD));
            kprintf("EHCI: bulk %s endpoint %u failed (%u so far), token %x "
                    "overlay %x current %x usbsts %x usbcmd %x\n",
                    in ? "in" : "out", (unsigned)endpoint, seen, (unsigned)token,
                    (unsigned)overlay, (unsigned)current,
                    (unsigned)status, (unsigned)command);
        }
        if ((token | overlay) & QTD_STATUS_HALTED) {
            if (clear_endpoint_halt(host, device, endpoint, in) == 0) {
                *toggle = 0;
            } else if (seen <= BULK_FAILURES_REPORTED) {
                kprintf("EHCI: the halt on endpoint %u would not clear\n",
                        (unsigned)endpoint);
            }
        }
        return -1;
    }

    uint32_t remaining = (token >> QTD_LENGTH_SHIFT) & 0x7FFFU;
    uint32_t moved = length > remaining ? length - remaining : 0;
    uint32_t packets = packet ? (moved + packet - 1U) / packet : 0;
    *toggle = (uint8_t)((*toggle + packets) & 1U);
    return 0;
}

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

    dma_page = (uint8_t *)dma_alloc_page(&dma_physical);
    if (!dma_page) {
        kprintf("EHCI: no DMA memory below 4 GiB\n");
        return -1;
    }
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

    delay_ns(PORT_POWER_SETTLE_NS);
    for (unsigned which = 0; which < controller_count; which++)
        enumerate_ports(&controllers[which]);

    if (ehci_storage_count()) usb_register_host(&ehci_host);
    return 0;
}
