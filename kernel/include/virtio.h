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

/* In the available ring's flags: "do not interrupt me". A driver that is
   watching the used ring anyway is asking to be told something it already
   knows, and being told costs the device a message and this machine a
   delivery. */
#define VIRTQ_AVAIL_F_NO_INTERRUPT 1U

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
    /* The one allocation the three rings live in, and how big it is. Held so
       the queue can be given back, which matters on the path where a device
       is found, set up, and then turns out not to work. */
    void *memory;
    uint64_t bytes;
    /* Set when the device raises an interrupt for this queue, which is what
       lets a waiter sleep instead of spinning. */
    int interrupt_driven;
    /*
     * The descriptors nobody is using, as a list threaded through their own
     * `next` fields, and how many requests are in the device's hands.
     *
     * A queue that only ever held one request needed none of this: it wrote
     * descriptor zero and waited. Holding several means knowing which are
     * free, and the device tells you a chain is finished by handing back its
     * head.
     */
    uint16_t free_head;
    uint16_t free_count;
    uint64_t posted;
    uint64_t completed;
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

/*
 * Hand the device a request and come back without waiting for it.
 *
 * The buffers must stay where they are, and stay untouched, until the device
 * has finished with them: it is reading them after this returns. What tells
 * you it has finished is virtio_queue_reclaim(), which is also the only thing
 * that gives the descriptors back, so a driver that never calls it will run
 * out of them.
 *
 * Fails when there are not enough free descriptors, which is the caller's cue
 * to reclaim or to drain.
 */
int virtio_queue_post(struct virtio_queue *queue, const struct virtio_buffer *buffers,
                      unsigned count, unsigned write_from);
/* Take back every chain the device has finished with. Returns how many. */
unsigned virtio_queue_reclaim(struct virtio_queue *queue);
/* Wait until nothing is outstanding. 0, or -1 if the device stopped answering. */
int virtio_queue_drain(struct virtio_queue *queue);
uint64_t virtio_queue_outstanding(const struct virtio_queue *queue);
int virtio_ring_alloc(struct virtio_queue *queue, uint16_t size);
void virtio_ring_free(struct virtio_queue *queue);

#endif
