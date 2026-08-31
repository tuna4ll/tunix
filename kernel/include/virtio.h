#ifndef TUNIX_VIRTIO_H
#define TUNIX_VIRTIO_H

#include <stdint.h>
#include "irq.h"
#include "pci.h"

/*
 * The modern (virtio 1.0) PCI transport and a split virtqueue.
 *
 * Requests are still waited out on the used ring. What virtio_pci_request_irq()
 * adds is a device that also *says* when it has finished one, which is the
 * half that was missing: a queue the driver has to look at to learn anything
 * cannot be drained by the device's own timing, only by the driver's.
 */

#define VIRTIO_VENDOR_ID 0x1AF4U

#define VIRTIO_STATUS_ACKNOWLEDGE 0x01U
#define VIRTIO_STATUS_DRIVER 0x02U
#define VIRTIO_STATUS_DRIVER_OK 0x04U
#define VIRTIO_STATUS_FEATURES_OK 0x08U
#define VIRTIO_STATUS_FAILED 0x80U

#define VIRTIO_F_VERSION_1 32U

#define VIRTQ_DESC_F_NEXT 1U
#define VIRTQ_DESC_F_WRITE 2U

struct virtq_desc {
    uint64_t address;
    uint32_t length;
    uint16_t flags;
    uint16_t next;
};

struct virtq_avail {
    uint16_t flags;
    uint16_t index;
    uint16_t ring[];
};

struct virtq_used_element {
    uint32_t id;
    uint32_t length;
};

struct virtq_used {
    uint16_t flags;
    uint16_t index;
    struct virtq_used_element ring[];
};

/* The spec fixes these layouts; natural alignment already produces them, and a
   mismatch would be a ring the device reads at the wrong stride. */
typedef char virtq_desc_size_check[(sizeof(struct virtq_desc) == 16) ? 1 : -1];
typedef char virtq_used_element_size_check[
    (sizeof(struct virtq_used_element) == 8) ? 1 : -1];

struct virtio_queue {
    uint16_t index;
    uint16_t size;
    uint16_t last_used;
    struct virtq_desc *descriptors;
    struct virtq_avail *available;
    struct virtq_used *used;
    volatile uint16_t *doorbell;
};

struct virtio_device {
    struct pci_device pci;
    volatile uint8_t *common;
    volatile uint8_t *notify;
    volatile uint8_t *isr;
    volatile uint8_t *config;
    uint32_t notify_multiplier;
    /* The vector the device was given, or 0 while it has none. Queues set up
       after this is non-zero are pointed at it. */
    unsigned vector;
};

/* Find, reset and take ownership of a virtio device, negotiating VERSION_1 plus
   whatever `features` asks for. `features_out` reports what the device agreed
   to. Leaves the device in DRIVER state; queues are set up after. */
int virtio_pci_attach(struct virtio_device *device, uint16_t device_id,
                      uint64_t features, uint64_t *features_out);
int virtio_pci_setup_queue(struct virtio_device *device, struct virtio_queue *queue,
                           uint16_t index);
void virtio_pci_set_driver_ok(struct virtio_device *device);

/*
 * Give the device an interrupt of its own, between attach and the first queue.
 *
 * One MSI-X entry covers the whole device: its configuration changes and every
 * queue on it. A device with several busy queues would want one each so they
 * can be told apart without reading anything, but nothing here has that yet,
 * and one vector is what makes the difference between an interrupt and none.
 *
 * Fails when the device has no MSI-X capability, when the vector pool is
 * empty, or when the device refuses the vector -- and a driver may carry on
 * regardless, since polling is what it did before.
 */
int virtio_pci_request_irq(struct virtio_device *device, const char *name,
                           irq_handler_fn handler, void *context);
void virtio_pci_set_failed(struct virtio_device *device);
uint32_t virtio_config_read32(const struct virtio_device *device, uint32_t offset);

/* Submit one descriptor chain and wait for the device to hand it back. The
   buffers are described by physical address; `write_from` is the index of the
   first device-writable one. Returns 0, or -1 on timeout. */
#define VIRTIO_MAX_CHAIN 16U

struct virtio_buffer {
    uint64_t physical;
    uint32_t length;
};

int virtio_queue_submit(struct virtio_queue *queue, const struct virtio_buffer *buffers,
                        unsigned count, unsigned write_from);
int virtio_ring_alloc(struct virtio_queue *queue, uint16_t size);

#endif
