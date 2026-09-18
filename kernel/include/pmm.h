#ifndef TUNIX_PMM_H
#define TUNIX_PMM_H

#include <stdint.h>

#include "boot.h"

#define PMM_PAGE_SIZE 4096ULL
#define PMM_DIRECT_MAP_LIMIT (8ULL * 1024ULL * 1024ULL * 1024ULL)

void pmm_init(const struct boot_memory_region *regions, uint32_t count);
void *pmm_alloc_page(void);
void *pmm_alloc_pages(uint64_t count, uint64_t alignment_bytes);
void *pmm_alloc_pages_below(uint64_t count, uint64_t alignment_bytes, uint64_t limit);
void pmm_free_pages(void *physical_address, uint64_t count);
void pmm_free_page(void *physical_address);
int pmm_page_ref(uint64_t physical);
uint32_t pmm_page_refcount(uint64_t physical);
uint64_t pmm_total_page_count(void);
uint64_t pmm_usable_page_count(void);
uint64_t pmm_free_page_count(void);
uint64_t pmm_release_reserved(uint64_t physical, uint64_t length);
uint64_t pmm_managed_limit(void);
int pmm_physical_range_managed(uint64_t physical, uint64_t length);
int pmm_page_is_allocated(uint64_t physical);

#endif
