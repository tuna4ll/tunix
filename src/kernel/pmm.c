#include "include/build_config.h"
#include <stddef.h>
#include <stdint.h>
#include "include/pmm.h"

#define KERNEL_BASE 0xFFFFFFFF80000000ULL

extern uint8_t kernel_end;
extern void kprintf(const char *fmt, ...);

#if TUNIX_DEBUG_LOGS
#define KDEBUG(...) kprintf(__VA_ARGS__)
#else
#define KDEBUG(...) do { } while (0)
#endif
extern void panic(const char *msg);

static uint8_t *bitmap;
static uint64_t total_pages;
static uint64_t free_pages;
static uint64_t next_hint;

static inline void bit_set(uint64_t page) {
    bitmap[page >> 3] |= (uint8_t)(1U << (page & 7));
}

static inline void bit_clear(uint64_t page) {
    bitmap[page >> 3] &= (uint8_t)~(1U << (page & 7));
}

static inline int bit_test(uint64_t page) {
    return (bitmap[page >> 3] & (uint8_t)(1U << (page & 7))) != 0;
}

static void reserve_page(uint64_t page) {
    if (page < total_pages && !bit_test(page)) {
        bit_set(page);
        free_pages--;
    }
}

void pmm_init(uint32_t mmap_count, uint64_t mmap_addr,
              uint64_t reserve_start, uint64_t reserve_size) {
    struct e820_entry *entries = (struct e820_entry *)mmap_addr;
    uint64_t highest = 0;

    for (uint32_t i = 0; i < mmap_count; i++) {
        if (entries[i].type != 1) continue;
        uint64_t end = entries[i].base + entries[i].length;
        if (end < entries[i].base) continue;
        if (end > PMM_DIRECT_MAP_LIMIT) end = PMM_DIRECT_MAP_LIMIT;
        if (end > highest) highest = end;
    }
    if (highest < 2 * 1024 * 1024ULL) panic("PMM: insufficient usable memory");

    total_pages = highest / PMM_PAGE_SIZE;
    uint64_t bitmap_bytes = (total_pages + 7) / 8;
    uint64_t bitmap_virtual = ((uint64_t)&kernel_end + 15ULL) & ~15ULL;
    uint64_t bitmap_physical = bitmap_virtual - KERNEL_BASE;
    bitmap = (uint8_t *)bitmap_virtual;

    for (uint64_t i = 0; i < bitmap_bytes; i++) bitmap[i] = 0xFF;
    free_pages = 0;

    for (uint32_t i = 0; i < mmap_count; i++) {
        if (entries[i].type != 1) continue;
        uint64_t start = (entries[i].base + PMM_PAGE_SIZE - 1) & ~(PMM_PAGE_SIZE - 1);
        uint64_t end = (entries[i].base + entries[i].length) & ~(PMM_PAGE_SIZE - 1);
        if (end > highest) end = highest;
        for (uint64_t address = start; address < end; address += PMM_PAGE_SIZE) {
            uint64_t page = address / PMM_PAGE_SIZE;
            if (page < total_pages && bit_test(page)) {
                bit_clear(page);
                free_pages++;
            }
        }
    }

    uint64_t reserved_end = bitmap_physical + bitmap_bytes;
    reserved_end = (reserved_end + PMM_PAGE_SIZE - 1) & ~(PMM_PAGE_SIZE - 1);
    if (reserved_end < 0x100000ULL) reserved_end = 0x100000ULL;

    for (uint64_t page = 0; page < reserved_end / PMM_PAGE_SIZE; page++) reserve_page(page);

    if (reserve_size) {
        uint64_t reserve_end = reserve_start + reserve_size;
        if (reserve_end < reserve_start) panic("PMM: reserve range overflow");
        uint64_t first_page = reserve_start / PMM_PAGE_SIZE;
        uint64_t last_page = (reserve_end + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE;
        for (uint64_t page = first_page; page < last_page; page++) reserve_page(page);
    }

    next_hint = reserved_end / PMM_PAGE_SIZE;

    KDEBUG("PMM: managed=%u MiB pages=%u free=%u\n",
            (unsigned)(highest / (1024 * 1024ULL)),
            (unsigned)total_pages,
            (unsigned)free_pages);
}


void *pmm_alloc_page(void) {
    if (!free_pages) return NULL;

    for (uint64_t pass = 0; pass < 2; pass++) {
        uint64_t begin = pass == 0 ? next_hint : 0;
        uint64_t end = pass == 0 ? total_pages : next_hint;
        for (uint64_t page = begin; page < end; page++) {
            if (!bit_test(page)) {
                bit_set(page);
                free_pages--;
                next_hint = page + 1;
                if (next_hint >= total_pages) next_hint = 0;
                return (void *)(page * PMM_PAGE_SIZE);
            }
        }
    }

    panic("PMM: bitmap/free-page invariant broken");
    return NULL;
}

void pmm_free_page(void *physical_address) {
    if (!physical_address) return;
    uint64_t address = (uint64_t)physical_address;
    if ((address & (PMM_PAGE_SIZE - 1)) || address >= total_pages * PMM_PAGE_SIZE) {
        panic("PMM: invalid free");
    }
    uint64_t page = address / PMM_PAGE_SIZE;
    if (!bit_test(page)) panic("PMM: double free");
    bit_clear(page);
    free_pages++;
    if (page < next_hint) next_hint = page;
}

uint64_t pmm_total_page_count(void) { return total_pages; }
uint64_t pmm_free_page_count(void) { return free_pages; }
uint64_t pmm_managed_limit(void) { return total_pages * PMM_PAGE_SIZE; }

int pmm_physical_range_managed(uint64_t physical, uint64_t length) {
    uint64_t limit = pmm_managed_limit();
    if (!length) return physical <= limit;
    if (physical >= limit || length > limit - physical) return 0;
    return 1;
}

int pmm_page_is_allocated(uint64_t physical) {
    if ((physical & (PMM_PAGE_SIZE - 1)) != 0 ||
        physical >= pmm_managed_limit()) return 0;
    return bit_test(physical / PMM_PAGE_SIZE);
}
