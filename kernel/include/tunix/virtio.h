#ifndef TUNIX_VIRTIO_H
#define TUNIX_VIRTIO_H

#include <stdint.h>
#include <tunix/irq.h>
#include <tunix/pci.h>

#define VIRTIO_VENDOR_ID 0x1AF4U

#define VIRTIO_STATUS_ACKNOWLEDGE 0x01U
#define VIRTIO_STATUS_DRIVER 0x02U
#define VIRTIO_STATUS_DRIVER_OK 0x04U
#define VIRTIO_STATUS_FEATURES_OK 0x08U
#define VIRTIO_STATUS_FAILED 0x80U

#define VIRTIO_F_VERSION_1 32U

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
    void *memory;
    uint64_t bytes;
    int interrupt_driven;
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
    volatile uint8_t *bars[6];
    unsigned vector;
};

int virtio_pci_attach(struct virtio_device *device, uint16_t device_id,
                      uint64_t features, uint64_t *features_out);
int virtio_pci_setup_queue(struct virtio_device *device, struct virtio_queue *queue,
                           uint16_t index);
void virtio_pci_set_driver_ok(struct virtio_device *device);

int virtio_pci_request_irq(struct virtio_device *device, const char *name,
                           irq_handler_fn handler, void *context);
void virtio_pci_set_failed(struct virtio_device *device);
uint32_t virtio_config_read32(const struct virtio_device *device, uint32_t offset);

#define VIRTIO_MAX_CHAIN 16U

struct virtio_buffer {
    uint64_t physical;
    uint32_t length;
};

int virtio_queue_submit(struct virtio_queue *queue, const struct virtio_buffer *buffers,
                        unsigned count, unsigned write_from);

int virtio_queue_post(struct virtio_queue *queue, const struct virtio_buffer *buffers,
                      unsigned count, unsigned write_from);
unsigned virtio_queue_reclaim(struct virtio_queue *queue);
int virtio_queue_take_used(struct virtio_queue *queue, uint64_t *address,
                           uint32_t *length);
int virtio_queue_drain(struct virtio_queue *queue);
uint64_t virtio_queue_outstanding(const struct virtio_queue *queue);
int virtio_ring_alloc(struct virtio_queue *queue, uint16_t size);
void virtio_ring_free(struct virtio_queue *queue);

#endif
