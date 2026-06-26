#ifndef TUNIX_PMM_H
#define TUNIX_PMM_H

#include <stdint.h>

#define PMM_PAGE_SIZE 4096ULL
#define PMM_DIRECT_MAP_LIMIT (1024ULL * 1024ULL * 1024ULL)

struct e820_entry {
    uint64_t base;
    uint64_t length;
    uint32_t type;
    uint32_t acpi;
} __attribute__((packed));

void pmm_init(uint32_t mmap_count, uint64_t mmap_addr,
              uint64_t reserve_start, uint64_t reserve_size);
void *pmm_alloc_page(void);
void pmm_free_page(void *physical_address);
uint64_t pmm_total_page_count(void);
uint64_t pmm_free_page_count(void);
uint64_t pmm_managed_limit(void);
int pmm_physical_range_managed(uint64_t physical, uint64_t length);
int pmm_page_is_allocated(uint64_t physical);

#endif
