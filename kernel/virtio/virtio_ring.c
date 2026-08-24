/*
 * A split virtqueue: three arrays the driver and the device share, with no
 * interrupt between them.
 *
 * The three rings get a page each rather than one shared allocation. The
 * physical allocator hands out single pages and nothing else, and modern virtio
 * lets every ring have its own address, so a page apiece is both the simplest
 * thing that works and the only contiguous thing available.
 */
#include <stddef.h>
#include <stdint.h>

#include "../include/kstring.h"
#include "../include/pmm.h"
#include "../include/time.h"
#include "../include/virtio.h"
#include "../include/vmm.h"

#define SUBMIT_TIMEOUT_NS (2ULL * 1000ULL * 1000ULL * 1000ULL)

static void *alloc_ring_page(void) {
    uint64_t physical = (uint64_t)pmm_alloc_page();
    if (!physical) return NULL;
    void *virtual_address = vmm_phys_to_virt(physical);
    memset(virtual_address, 0, 4096);
    return virtual_address;
}

int virtio_ring_alloc(struct virtio_queue *queue, uint16_t size) {
    if (!queue || !size || (size & (size - 1U))) return -1;
    if ((uint32_t)size * sizeof(struct virtq_desc) > 4096U) return -1;

    memset(queue, 0, sizeof(*queue));
    queue->descriptors = (struct virtq_desc *)alloc_ring_page();
    queue->available = (struct virtq_avail *)alloc_ring_page();
    queue->used = (struct virtq_used *)alloc_ring_page();
    if (!queue->descriptors || !queue->available || !queue->used) {
        if (queue->descriptors) pmm_free_page((void *)vmm_virt_to_phys_direct(queue->descriptors));
        if (queue->available) pmm_free_page((void *)vmm_virt_to_phys_direct(queue->available));
        if (queue->used) pmm_free_page((void *)vmm_virt_to_phys_direct(queue->used));
        memset(queue, 0, sizeof(*queue));
        return -1;
    }
    queue->size = size;
    return 0;
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

    queue->available->ring[queue->available->index % queue->size] = 0;
    __sync_synchronize();
    queue->available->index++;
    __sync_synchronize();
    *queue->doorbell = queue->index;

    /* The device writes the used index behind the compiler's back, so it has to
       be re-read on every lap rather than cached in a register. */
    volatile uint16_t *used_index = (volatile uint16_t *)&queue->used->index;
    uint64_t deadline = time_uptime_ns() + SUBMIT_TIMEOUT_NS;
    while (*used_index == queue->last_used) {
        if (time_uptime_ns() > deadline) return -1;
        __asm__ volatile("pause");
    }
    __sync_synchronize();
    queue->last_used = *used_index;
    return 0;
}
