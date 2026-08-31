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
    return 0;
}

void virtio_ring_free(struct virtio_queue *queue) {
    if (!queue || !queue->memory) return;
    dma_free(queue->memory, queue->bytes);
    memset(queue, 0, sizeof(*queue));
}

/*
 * One request at a time. Descriptors are taken from the head of the table and
 * the caller is kept waiting until the device gives them back, so there is
 * nothing to allocate and nothing outstanding to track.
 */
int virtio_queue_submit(struct virtio_queue *queue, const struct virtio_buffer *buffers,
                        unsigned count, unsigned write_from) {
    if (!queue || !queue->size || !queue->doorbell || !buffers || !count) return -1;
    if (count > VIRTIO_MAX_CHAIN || count > queue->size) return -1;

    for (unsigned index = 0; index < count; index++) {
        queue->descriptors[index].address = buffers[index].physical;
        queue->descriptors[index].length = buffers[index].length;
        queue->descriptors[index].flags =
            (uint16_t)((index + 1U < count ? VIRTQ_DESC_F_NEXT : 0U) |
                       (index >= write_from ? VIRTQ_DESC_F_WRITE : 0U));
        queue->descriptors[index].next = (uint16_t)(index + 1U);
    }

    /* Silence first, watch second. The wait below spins before it sleeps, and
       for everything that finishes inside the spin an interrupt is pure cost:
       a message from the device, a vector, a trip through the dispatcher, to
       announce something already visible in the ring. */
    queue->available->flags = VIRTQ_AVAIL_F_NO_INTERRUPT;
    queue->available->ring[queue->available->index % queue->size] = 0;
    __sync_synchronize();
    queue->available->index++;
    __sync_synchronize();
    *queue->doorbell = queue->index;

    /* The device writes the used index behind the compiler's back, so it has to
       be re-read on every lap rather than cached in a register. */
    volatile uint16_t *used_index = (volatile uint16_t *)&queue->used->index;
    uint64_t deadline = time_uptime_ns() + SUBMIT_TIMEOUT_NS;
    /*
     * Still a spin, and it has to be, though the device could now say when it
     * is done.
     *
     * Sleeping here would hand back a processor that is holding the kernel
     * lock, so nothing else could run on any of the others either: the machine
     * would wait exactly as long, having also stopped. Measured, and it is not
     * theoretical -- a halt in this loop cost SuperTuxKart its whole start-up.
     * The wait becomes a sleep when the lock this path holds is no longer the
     * whole kernel's.
     */
    while (*used_index == queue->last_used) {
        if (time_uptime_ns() > deadline) return -1;
        __asm__ volatile("pause");
    }
    __sync_synchronize();
    queue->last_used = *used_index;
    return 0;
}
