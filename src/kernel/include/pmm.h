#ifndef TUNIX_PMM_H
#define TUNIX_PMM_H

#include <stdint.h>

#define PMM_PAGE_SIZE 4096ULL
/*
 * How much physical memory the machine may use.
 *
 * This used to be 1792 MiB, and not by choice: the direct map started at
 * KERNEL_BASE and the framebuffer window sat exactly that far above it. The
 * map has its own PML4 entry now, so what remains is a limit on the bookkeeping
 * rather than on the address space -- the allocator's bitmap and per-page
 * reference counts are reserved after the kernel image by the linker script,
 * and they cost about 544 KiB for every gigabyte. 8 GiB needs 4.25 MiB of the
 * 8 MiB reserved there; raising this means raising that too.
 *
 * The structures are sized from the memory map at boot, not from this, so a
 * machine with 2 GiB pays for 2 GiB.
 */
#define PMM_DIRECT_MAP_LIMIT (8ULL * 1024ULL * 1024ULL * 1024ULL)

struct e820_entry {
    uint64_t base;
    uint64_t length;
    uint32_t type;
    uint32_t acpi;
} __attribute__((packed));

void pmm_init(uint32_t mmap_count, uint64_t mmap_addr,
              uint64_t reserve_start, uint64_t reserve_size);
void *pmm_alloc_page(void);
/* Drops one reference; the page returns to the allocator at the last one. */
void pmm_free_page(void *physical_address);
/* Share an allocated page with another owner. 0 on success, -1 if the page is
   not allocated or has saturated its reference count. */
int pmm_page_ref(uint64_t physical);
uint32_t pmm_page_refcount(uint64_t physical);
/* Every page up to the highest usable address, holes in the middle included. */
uint64_t pmm_total_page_count(void);
/* Only the pages that are RAM. This is the one to report memory against: the
   difference between the two is address space the machine does not have. */
uint64_t pmm_usable_page_count(void);
uint64_t pmm_free_page_count(void);
/* Return a range reserved at boot, once nothing points into it any more.
   Answers with the number of pages that actually went back. */
uint64_t pmm_release_reserved(uint64_t physical, uint64_t length);
uint64_t pmm_managed_limit(void);
int pmm_physical_range_managed(uint64_t physical, uint64_t length);
int pmm_page_is_allocated(uint64_t physical);

#endif
