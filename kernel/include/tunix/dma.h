#ifndef TUNIX_DMA_H
#define TUNIX_DMA_H

#include <stdint.h>

#define DMA_LIMIT_32BIT 0x100000000ULL

void *dma_alloc(uint64_t bytes, uint64_t alignment, uint64_t *physical);

void *dma_alloc_below(uint64_t bytes, uint64_t alignment, uint64_t limit,
                      uint64_t *physical);

void dma_free(void *pointer, uint64_t bytes);

#endif
