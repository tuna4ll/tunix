/*
 * The virtio 1.0 PCI transport.
 *
 * A modern virtio device does not put its registers at a fixed offset in a BAR.
 * It publishes a chain of vendor-specific PCI capabilities, each naming a BAR,
 * an offset and a length, and the driver has to walk that chain to find out
 * where the common configuration, the notification area and the device's own
 * configuration actually live.
 */
#include <stddef.h>
#include <stdint.h>

#include "../../include/kstring.h"
#include "../../include/pci.h"
#include "../../include/time.h"
#include "../../include/virtio.h"
#include "../../include/vmm.h"

#define PCI_STATUS_CAPABILITIES (1U << 4)
#define PCI_CAPABILITY_POINTER 0x34U
#define PCI_CAP_ID_VENDOR 0x09U

#define VIRTIO_PCI_CAP_COMMON_CFG 1U
#define VIRTIO_PCI_CAP_NOTIFY_CFG 2U
#define VIRTIO_PCI_CAP_ISR_CFG 3U
#define VIRTIO_PCI_CAP_DEVICE_CFG 4U

#define COMMON_DEVICE_FEATURE_SELECT 0x00U
#define COMMON_DEVICE_FEATURE 0x04U
#define COMMON_DRIVER_FEATURE_SELECT 0x08U
#define COMMON_DRIVER_FEATURE 0x0CU
#define COMMON_CONFIG_MSIX_VECTOR 0x10U
#define COMMON_NUM_QUEUES 0x12U
#define COMMON_DEVICE_STATUS 0x14U
#define COMMON_QUEUE_SELECT 0x16U
#define COMMON_QUEUE_SIZE 0x18U
#define COMMON_QUEUE_MSIX_VECTOR 0x1AU
#define COMMON_QUEUE_ENABLE 0x1CU
#define COMMON_QUEUE_NOTIFY_OFF 0x1EU
#define COMMON_QUEUE_DESC 0x20U
#define COMMON_QUEUE_DRIVER 0x28U
#define COMMON_QUEUE_DEVICE 0x30U

/* The slot this driver owns in the shared device window; see vmm.h. One 64 KiB
   sub-slot per BAR index, which is more than the 16 KiB QEMU actually uses. */
#define VIRTIO_MMIO_VIRTUAL_BASE (DEVICE_MMIO_VIRTUAL_BASE + 0x00600000ULL)
#define VIRTIO_MMIO_BAR_BYTES 0x10000ULL
#define VIRTIO_MMIO_BAR_COUNT 6U

/* What the device writes back when it could not take the vector, and the only
   way it reports that: the write itself is silent. */
#define MSIX_NO_VECTOR 0xFFFFU

#define QUEUE_SIZE_MAX 64U
#define RESET_TIMEOUT_NS (500ULL * 1000ULL * 1000ULL)

static uint8_t mapped_bars;

static uint8_t config_read8(const struct pci_device *pci, uint8_t offset) {
    uint32_t value = pci_config_read32(pci->bus, pci->slot, pci->function,
                                       (uint8_t)(offset & 0xFCU));
    return (uint8_t)(value >> ((offset & 3U) * 8U));
}

static uint32_t config_read32_at(const struct pci_device *pci, uint8_t offset) {
    return pci_config_read32(pci->bus, pci->slot, pci->function, offset);
}

static void write32(volatile uint8_t *base, uint32_t offset, uint32_t value) {
    *(volatile uint32_t *)(base + offset) = value;
}

static uint32_t read32(volatile uint8_t *base, uint32_t offset) {
    return *(volatile uint32_t *)(base + offset);
}

static void write16(volatile uint8_t *base, uint32_t offset, uint16_t value) {
    *(volatile uint16_t *)(base + offset) = value;
}

static uint16_t read16(volatile uint8_t *base, uint32_t offset) {
    return *(volatile uint16_t *)(base + offset);
}

static void write8(volatile uint8_t *base, uint32_t offset, uint8_t value) {
    *(base + offset) = value;
}

static uint8_t read8(volatile uint8_t *base, uint32_t offset) {
    return *(base + offset);
}

/* 64-bit registers are written as two halves: the low one first, so the device
   never sees an address made of one new dword and one stale one. */
static void write64(volatile uint8_t *base, uint32_t offset, uint64_t value) {
    write32(base, offset, (uint32_t)value);
    write32(base, offset + 4U, (uint32_t)(value >> 32));
}

static volatile uint8_t *map_bar(const struct pci_device *pci, unsigned index) {
    if (index >= VIRTIO_MMIO_BAR_COUNT) return NULL;
    uint64_t physical = pci_bar_address(pci, index);
    if (!physical || (physical & 0xFFFULL)) return NULL;

    uint64_t virtual_base = VIRTIO_MMIO_VIRTUAL_BASE + (uint64_t)index * VIRTIO_MMIO_BAR_BYTES;
    if (!(mapped_bars & (1U << index))) {
        uint64_t cr3 = vmm_kernel_cr3();
        for (uint64_t offset = 0; offset < VIRTIO_MMIO_BAR_BYTES; offset += 4096ULL) {
            if (vmm_map_page_in(cr3, virtual_base + offset, physical + offset,
                                PAGE_WRITE | PAGE_DEVICE | PAGE_UNCACHED | PAGE_NX) != 0)
                return NULL;
        }
        mapped_bars |= (uint8_t)(1U << index);
    }
    return (volatile uint8_t *)virtual_base;
}

static void set_status(struct virtio_device *device, uint8_t bits) {
    uint8_t status = read8(device->common, COMMON_DEVICE_STATUS);
    write8(device->common, COMMON_DEVICE_STATUS, (uint8_t)(status | bits));
}

static int walk_capabilities(struct virtio_device *device) {
    const struct pci_device *pci = &device->pci;
    uint32_t status_command = config_read32_at(pci, 0x04);
    if (!((status_command >> 16) & PCI_STATUS_CAPABILITIES)) return -1;

    uint8_t offset = config_read8(pci, PCI_CAPABILITY_POINTER) & 0xFCU;
    for (unsigned guard = 0; offset && guard < 48U; guard++) {
        if (config_read8(pci, offset) == PCI_CAP_ID_VENDOR &&
            config_read8(pci, (uint8_t)(offset + 2U)) >= 16U) {
            uint8_t type = config_read8(pci, (uint8_t)(offset + 3U));
            uint8_t bar = config_read8(pci, (uint8_t)(offset + 4U));
            uint32_t within = config_read32_at(pci, (uint8_t)(offset + 8U));
            volatile uint8_t *base = map_bar(pci, bar);
            if (base && within < VIRTIO_MMIO_BAR_BYTES) {
                switch (type) {
                case VIRTIO_PCI_CAP_COMMON_CFG: device->common = base + within; break;
                case VIRTIO_PCI_CAP_NOTIFY_CFG:
                    device->notify = base + within;
                    device->notify_multiplier = config_read32_at(pci, (uint8_t)(offset + 16U));
                    break;
                case VIRTIO_PCI_CAP_ISR_CFG: device->isr = base + within; break;
                case VIRTIO_PCI_CAP_DEVICE_CFG: device->config = base + within; break;
                default: break;
                }
            }
        }
        offset = config_read8(pci, (uint8_t)(offset + 1U)) & 0xFCU;
    }
    return (device->common && device->notify) ? 0 : -1;
}

static int negotiate(struct virtio_device *device, uint64_t wanted, uint64_t *agreed) {
    uint64_t offered = 0;
    for (unsigned half = 0; half < 2U; half++) {
        write32(device->common, COMMON_DEVICE_FEATURE_SELECT, half);
        offered |= (uint64_t)read32(device->common, COMMON_DEVICE_FEATURE) << (half * 32U);
    }
    /* VERSION_1 is not optional: without it the device stays in the legacy
       layout this transport cannot speak. */
    if (!(offered & (1ULL << VIRTIO_F_VERSION_1))) return -1;

    uint64_t accepted = (offered & wanted) | (1ULL << VIRTIO_F_VERSION_1);
    for (unsigned half = 0; half < 2U; half++) {
        write32(device->common, COMMON_DRIVER_FEATURE_SELECT, half);
        write32(device->common, COMMON_DRIVER_FEATURE, (uint32_t)(accepted >> (half * 32U)));
    }
    set_status(device, VIRTIO_STATUS_FEATURES_OK);
    if (!(read8(device->common, COMMON_DEVICE_STATUS) & VIRTIO_STATUS_FEATURES_OK)) return -1;
    if (agreed) *agreed = accepted;
    return 0;
}

int virtio_pci_attach(struct virtio_device *device, uint16_t device_id,
                      uint64_t features, uint64_t *features_out) {
    if (!device) return -1;
    memset(device, 0, sizeof(*device));
    if (pci_find_device(VIRTIO_VENDOR_ID, device_id, &device->pci) != 0) return -1;
    if (walk_capabilities(device) != 0) return -1;

    write8(device->common, COMMON_DEVICE_STATUS, 0);
    uint64_t deadline = time_uptime_ns() + RESET_TIMEOUT_NS;
    while (read8(device->common, COMMON_DEVICE_STATUS) != 0) {
        if (time_uptime_ns() > deadline) return -1;
    }
    set_status(device, VIRTIO_STATUS_ACKNOWLEDGE);
    set_status(device, VIRTIO_STATUS_DRIVER);
    if (negotiate(device, features, features_out) != 0) {
        virtio_pci_set_failed(device);
        return -1;
    }
    pci_enable_bus_mastering(&device->pci);
    return 0;
}

int virtio_pci_setup_queue(struct virtio_device *device, struct virtio_queue *queue,
                           uint16_t index) {
    if (!device || !queue) return -1;
    if (index >= read16(device->common, COMMON_NUM_QUEUES)) return -1;

    write16(device->common, COMMON_QUEUE_SELECT, index);
    uint16_t size = read16(device->common, COMMON_QUEUE_SIZE);
    if (!size) return -1;
    if (size > QUEUE_SIZE_MAX) size = QUEUE_SIZE_MAX;

    if (virtio_ring_alloc(queue, size) != 0) return -1;
    queue->index = index;

    write16(device->common, COMMON_QUEUE_SIZE, size);
    /* While this queue is the selected one, and only then: the vector register
       is per-queue but reached through the same window as everything else. */
    if (device->vector) {
        write16(device->common, COMMON_QUEUE_MSIX_VECTOR, 0);
        if (read16(device->common, COMMON_QUEUE_MSIX_VECTOR) == MSIX_NO_VECTOR)
            device->vector = 0;
    }
    write64(device->common, COMMON_QUEUE_DESC, vmm_virt_to_phys_direct(queue->descriptors));
    write64(device->common, COMMON_QUEUE_DRIVER, vmm_virt_to_phys_direct(queue->available));
    write64(device->common, COMMON_QUEUE_DEVICE, vmm_virt_to_phys_direct(queue->used));

    uint32_t notify_offset = read16(device->common, COMMON_QUEUE_NOTIFY_OFF);
    queue->doorbell = (volatile uint16_t *)(device->notify +
                                            notify_offset * device->notify_multiplier);
    write16(device->common, COMMON_QUEUE_ENABLE, 1);
    return 0;
}

int virtio_pci_request_irq(struct virtio_device *device, const char *name,
                           irq_handler_fn handler, void *context) {
    if (!device || !device->common || !handler) return -1;
    if (pci_msix_enable(&device->pci) != 0) return -1;

    unsigned vector = irq_request(name, handler, context);
    if (!vector) return -1;
    if (pci_msix_bind(&device->pci, 0, vector) != 0) return -1;

    /* Entry 0 answers for the device's configuration as well as its queues.
       The read back is the whole check: writing a vector a device cannot take
       leaves NO_VECTOR here and nothing else says so. */
    write16(device->common, COMMON_CONFIG_MSIX_VECTOR, 0);
    if (read16(device->common, COMMON_CONFIG_MSIX_VECTOR) == MSIX_NO_VECTOR)
        return -1;

    device->vector = vector;
    return 0;
}

void virtio_pci_set_driver_ok(struct virtio_device *device) {
    set_status(device, VIRTIO_STATUS_DRIVER_OK);
}

void virtio_pci_set_failed(struct virtio_device *device) {
    set_status(device, VIRTIO_STATUS_FAILED);
}

uint32_t virtio_config_read32(const struct virtio_device *device, uint32_t offset) {
    if (!device || !device->config) return 0;
    return read32(device->config, offset);
}
