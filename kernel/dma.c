#include <stddef.h>
#include <stdint.h>

#include "include/dma.h"
#include "include/kstring.h"
#include "include/pmm.h"
#include "include/vmm.h"

static uint64_t pages_for(uint64_t bytes) {
    return (bytes + PMM_PAGE_SIZE - 1ULL) / PMM_PAGE_SIZE;
}

void *dma_alloc_below(uint64_t bytes, uint64_t alignment, uint64_t limit,
                      uint64_t *physical) {
    if (!bytes || !physical) return NULL;
    if (alignment < PMM_PAGE_SIZE) alignment = PMM_PAGE_SIZE;

    uint64_t count = pages_for(bytes);
    void *address = pmm_alloc_pages_below(count, alignment, limit);
    if (!address) return NULL;

    void *mapped = vmm_phys_to_virt((uint64_t)address);
    memset(mapped, 0, count * PMM_PAGE_SIZE);
    *physical = (uint64_t)address;
    return mapped;
}

void *dma_alloc(uint64_t bytes, uint64_t alignment, uint64_t *physical) {
    return dma_alloc_below(bytes, alignment, 0, physical);
}

void dma_free(void *pointer, uint64_t bytes) {
    if (!pointer || !bytes) return;
    pmm_free_pages((void *)vmm_virt_to_phys_direct(pointer), pages_for(bytes));
}
