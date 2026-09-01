/*
 * A split virtqueue: three arrays the driver and the device share.
 *
 * They used to get a page each, because a page was the largest contiguous run
 * the physical allocator could produce, and that decided the queue size: 256
 * descriptors are 4 KiB exactly, leaving no room for the ring's own header, so
 * the ceiling was 64. With dma_alloc() the three sit in one allocation at the
 * alignments the spec asks for, and the size is the device's to choose again.
 */
#include <stddef.h>
#include <stdint.h>

#include "../../include/dma.h"
#include "../../include/kstring.h"
#include "../../include/time.h"
#include "../../include/virtio.h"
#include "../../include/vmm.h"

extern void kprintf(const char *fmt, ...);

#define SUBMIT_TIMEOUT_NS (2ULL * 1000ULL * 1000ULL * 1000ULL)

/* The spec asks for 16, 2 and 4 byte alignment for the three rings. Sixteen
   throughout costs a handful of bytes and makes the arithmetic below one rule
   instead of three. */
#define RING_ALIGNMENT 16ULL

static uint64_t aligned(uint64_t value) {
    return (value + RING_ALIGNMENT - 1ULL) & ~(RING_ALIGNMENT - 1ULL);
}

int virtio_ring_alloc(struct virtio_queue *queue, uint16_t size) {
    if (!queue || !size || (size & (size - 1U))) return -1;

    memset(queue, 0, sizeof(*queue));

    /* Both rings carry one more 16-bit field than their arrays: the event
       index the other side publishes. Nothing here uses those, but the device
       reads and writes them regardless, so the space has to be there. */
    uint64_t descriptor_bytes = aligned((uint64_t)size * sizeof(struct virtq_desc));
    uint64_t available_bytes = aligned(6ULL + 2ULL * size);
    uint64_t used_bytes = aligned(6ULL + 8ULL * size);
    uint64_t bytes = descriptor_bytes + available_bytes + used_bytes;

    uint64_t physical = 0;
    uint8_t *memory = (uint8_t *)dma_alloc(bytes, RING_ALIGNMENT, &physical);
    if (!memory) return -1;

    queue->memory = memory;
    queue->bytes = bytes;
    queue->descriptors = (struct virtq_desc *)memory;
    queue->available = (struct virtq_avail *)(memory + descriptor_bytes);
    queue->used = (struct virtq_used *)(memory + descriptor_bytes + available_bytes);
    queue->size = size;

    /* Every descriptor free, in one list. */
    for (uint16_t index = 0; index < size; index++)
        queue->descriptors[index].next = (uint16_t)(index + 1U);
    queue->free_head = 0;
    queue->free_count = size;
    return 0;
}

/* Take `count` descriptors off the free list and return the head, or -1. */
static int take_descriptors(struct virtio_queue *queue, unsigned count) {
    if (queue->free_count < count) return -1;
    int head = queue->free_head;
    uint16_t last = queue->free_head;
    for (unsigned index = 0; index < count; index++) {
        last = queue->free_head;
        queue->free_head = queue->descriptors[last].next;
        queue->free_count--;
    }
    (void)last;
    return head;
}

/* Put a chain back, following it to its end. */
static void give_descriptors_back(struct virtio_queue *queue, uint16_t head) {
    uint16_t index = head;
    for (unsigned guard = 0; guard < queue->size; guard++) {
        queue->free_count++;
        if (!(queue->descriptors[index].flags & VIRTQ_DESC_F_NEXT)) break;
        index = queue->descriptors[index].next;
    }
    /* The chain's own links are already right; only its tail has to point at
       what used to be free. */
    queue->descriptors[index].next = queue->free_head;
    queue->free_head = head;
}

int virtio_queue_post(struct virtio_queue *queue, const struct virtio_buffer *buffers,
                      unsigned count, unsigned write_from) {
    if (!queue || !queue->size || !queue->doorbell || !buffers || !count) return -1;
    if (count > VIRTIO_MAX_CHAIN || count > queue->size) return -1;

    int head = take_descriptors(queue, count);
    if (head < 0) return -1;

    uint16_t index = (uint16_t)head;
    for (unsigned position = 0; position < count; position++) {
        struct virtq_desc *descriptor = &queue->descriptors[index];
        descriptor->address = buffers[position].physical;
        descriptor->length = buffers[position].length;
        descriptor->flags =
            (uint16_t)((position + 1U < count ? VIRTQ_DESC_F_NEXT : 0U) |
                       (position >= write_from ? VIRTQ_DESC_F_WRITE : 0U));
        index = descriptor->next;
    }

    /* Silence first, watch second: nothing here waits for an interrupt, and
       one raised for a completion the driver will notice anyway is a message,
       a vector and a trip through the dispatcher spent saying so. */
    queue->available->flags = VIRTQ_AVAIL_F_NO_INTERRUPT;
    queue->available->ring[queue->available->index % queue->size] = (uint16_t)head;
    __sync_synchronize();
    queue->available->index++;
    __sync_synchronize();
    *queue->doorbell = queue->index;
    queue->posted++;
    return 0;
}

unsigned virtio_queue_reclaim(struct virtio_queue *queue) {
    if (!queue || !queue->size) return 0;
    volatile uint16_t *used_index = (volatile uint16_t *)&queue->used->index;
    unsigned taken = 0;
    while (*used_index != queue->last_used) {
        __sync_synchronize();
        uint32_t head = queue->used->ring[queue->last_used % queue->size].id;
        if (head < queue->size) give_descriptors_back(queue, (uint16_t)head);
        queue->last_used++;
        queue->completed++;
        taken++;
    }
    return taken;
}

uint64_t virtio_queue_outstanding(const struct virtio_queue *queue) {
    return queue ? queue->posted - queue->completed : 0;
}

int virtio_queue_drain(struct virtio_queue *queue) {
    if (!queue) return -1;
    uint64_t deadline = time_uptime_ns() + SUBMIT_TIMEOUT_NS;
    while (virtio_queue_outstanding(queue)) {
        virtio_queue_reclaim(queue);
        if (!virtio_queue_outstanding(queue)) break;
        if (time_uptime_ns() > deadline) {
            kprintf("VIRTIO drain gave up with %u outstanding\n",
                    (unsigned)virtio_queue_outstanding(queue));
            return -1;
        }
        __asm__ volatile("pause");
    }
    return 0;
}

void virtio_ring_free(struct virtio_queue *queue) {
    if (!queue || !queue->memory) return;
    dma_free(queue->memory, queue->bytes);
    memset(queue, 0, sizeof(*queue));
}

/*
 * Post one request and wait for the device to finish everything outstanding.
 *
 * Which is more than this request when others are in flight, and that is the
 * point: this queue is answered in order, so waiting for the last thing posted
 * is waiting for all of them. Callers that read a response need exactly that
 * guarantee.
 */
int virtio_queue_submit(struct virtio_queue *queue, const struct virtio_buffer *buffers,
                        unsigned count, unsigned write_from) {
    if (virtio_queue_post(queue, buffers, count, write_from) != 0) {
        /* Out of descriptors: everything in flight has to come back first. */
        if (virtio_queue_drain(queue) != 0) return -1;
        if (virtio_queue_post(queue, buffers, count, write_from) != 0) return -1;
    }
    return virtio_queue_drain(queue);
}
