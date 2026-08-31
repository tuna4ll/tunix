#ifndef TUNIX_DMA_H
#define TUNIX_DMA_H

#include <stdint.h>

/*
 * Memory a device can be pointed at.
 *
 * Every driver here used to answer this question for itself, and there were
 * only two answers available. One is a static array in the kernel image with an
 * alignment attribute, which is why ata.c, rtl8139.c, ahci.c and nvme.c each
 * carry one: it is contiguous because the image is, and it costs its full size
 * on every machine whether the device is present or not. The other is
 * pmm_alloc_page() one page at a time, which is contiguous only up to 4 KiB --
 * and that limit is not a comment anywhere, it is the shape the drivers took to
 * live inside it. virtio's rings are a page each because of it.
 *
 * What a device actually needs is an address it can be given with a length,
 * where the whole length is one run of physical memory. That is what this is.
 *
 * There is no unmapping and no bounce buffering: the direct map already covers
 * every page of RAM, so the virtual address returned is simply where that
 * physical memory already was, and virt-to-phys is a subtraction. On a machine
 * with an IOMMU in the way this is where the translation would go.
 */

/*
 * `bytes` of zeroed, physically contiguous memory, aligned to `alignment`
 * (rounded up to a page; 0 means page alignment). The physical address the
 * device is given is written to `physical`, which may not be NULL -- a caller
 * that does not need it does not need this allocator.
 *
 * NULL when there is no run that long, which is not the same as being out of
 * memory: a fragmented machine can fail this and still have gigabytes free.
 */
void *dma_alloc(uint64_t bytes, uint64_t alignment, uint64_t *physical);

/* Give it back. `bytes` must be what was asked for. */
void dma_free(void *pointer, uint64_t bytes);

#endif
