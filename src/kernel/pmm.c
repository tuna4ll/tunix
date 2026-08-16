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
/*
 * One reference count per physical page, so a page can be shared by several
 * address spaces. Copy-on-write fork is the only producer of sharing today:
 * vmm_clone_address_space() maps the parent's pages into the child instead of
 * copying them and takes a reference on each.
 *
 * A flat array costs 2 bytes per 4 KiB page -- 512 KiB at the 1 GiB direct-map
 * limit -- which buys an O(1) lookup with no allocation on the fault path,
 * where a copy-on-write break cannot afford to fail.
 */
static uint16_t *refcounts;
static uint64_t total_pages;
/*
 * Pages that are actually RAM, which is not the same as total_pages.
 *
 * total_pages spans everything up to the highest usable address, and on a
 * machine whose firmware splits RAM around the PCI hole that includes the hole:
 * a 4 GiB QEMU guest has RAM at 0-3 GiB and 4-5 GiB, so the top is 5 GiB and a
 * gigabyte in the middle is not memory at all. Those pages start out marked
 * allocated and never come free, so anything reporting total minus free counts
 * a gigabyte of nothing as permanently in use.
 */
static uint64_t usable_pages;
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
    /* Summed, not taken from the top of the range: firmware splits RAM around
       the PCI hole, so on a 4 GiB machine the highest usable address is 5 GiB
       and reporting that would claim a gigabyte the machine does not have. */
    uint64_t installed = 0;
    uint64_t usable = 0;

    for (uint32_t i = 0; i < mmap_count; i++) {
        if (entries[i].type != 1) continue;
        uint64_t end = entries[i].base + entries[i].length;
        if (end < entries[i].base) continue;
        installed += entries[i].length;
        if (end > PMM_DIRECT_MAP_LIMIT) end = PMM_DIRECT_MAP_LIMIT;
        if (end > entries[i].base) usable += end - entries[i].base;
        if (end > highest) highest = end;
    }
    if (highest < 2 * 1024 * 1024ULL) panic("PMM: insufficient usable memory");

    total_pages = highest / PMM_PAGE_SIZE;
    uint64_t bitmap_bytes = (total_pages + 7) / 8;
    uint64_t bitmap_virtual = ((uint64_t)&kernel_end + 15ULL) & ~15ULL;
    bitmap = (uint8_t *)bitmap_virtual;

    /* The reference counts sit immediately after the allocation bitmap; both
       are reserved below so no allocation can ever hand them out. */
    uint64_t refcount_virtual = (bitmap_virtual + bitmap_bytes + 15ULL) & ~15ULL;
    uint64_t refcount_bytes = total_pages * sizeof(uint16_t);
    refcounts = (uint16_t *)refcount_virtual;

    for (uint64_t i = 0; i < bitmap_bytes; i++) bitmap[i] = 0xFF;
    for (uint64_t i = 0; i < total_pages; i++) refcounts[i] = 0;
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

    /* Everything the loop above cleared, before any of it is reserved again:
       that is exactly the set of pages backed by RAM. */
    usable_pages = free_pages;

    uint64_t reserved_end = (refcount_virtual - KERNEL_BASE) + refcount_bytes;
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

    /* Not behind the debug flag: how much of the machine's memory is actually
       usable is the first thing anyone wants to know, and it was silently
       capped for a long time. The ceiling is printed next to it so a machine
       that has more than the kernel will take says so plainly. */
    kprintf("PMM: %u MiB usable of %u MiB installed, ceiling %u MiB\n",
            (unsigned)(usable / (1024 * 1024ULL)),
            (unsigned)(installed / (1024 * 1024ULL)),
            (unsigned)(PMM_DIRECT_MAP_LIMIT / (1024 * 1024ULL)));
}


void *pmm_alloc_page(void) {
    if (!free_pages) return NULL;

    for (uint64_t pass = 0; pass < 2; pass++) {
        uint64_t begin = pass == 0 ? next_hint : 0;
        uint64_t end = pass == 0 ? total_pages : next_hint;
        for (uint64_t page = begin; page < end; page++) {
            if (!bit_test(page)) {
                bit_set(page);
                refcounts[page] = 1;
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

/*
 * Drops one reference and only returns the page to the allocator when the last
 * one goes away. Every existing caller was written when a page had exactly one
 * owner, and for those pages the behaviour is unchanged; shared pages simply
 * survive until the last address space holding them is torn down.
 */
void pmm_free_page(void *physical_address) {
    if (!physical_address) return;
    uint64_t address = (uint64_t)physical_address;
    if ((address & (PMM_PAGE_SIZE - 1)) || address >= total_pages * PMM_PAGE_SIZE) {
        panic("PMM: invalid free");
    }
    uint64_t page = address / PMM_PAGE_SIZE;
    if (!bit_test(page)) panic("PMM: double free");
    if (refcounts[page] > 1) {
        refcounts[page]--;
        return;
    }
    refcounts[page] = 0;
    bit_clear(page);
    free_pages++;
    if (page < next_hint) next_hint = page;
}

/*
 * Take an extra reference on an already-allocated page. Refuses rather than
 * wraps if a page somehow reaches 65535 sharers: the caller (the fork path)
 * falls back to copying, which is always correct, just slower.
 */
int pmm_page_ref(uint64_t physical) {
    if (!pmm_page_is_allocated(physical)) return -1;
    uint64_t page = physical / PMM_PAGE_SIZE;
    if (refcounts[page] == 0xFFFFU) return -1;
    refcounts[page]++;
    return 0;
}

uint32_t pmm_page_refcount(uint64_t physical) {
    if (!pmm_page_is_allocated(physical)) return 0;
    return refcounts[physical / PMM_PAGE_SIZE];
}

uint64_t pmm_total_page_count(void) { return total_pages; }
uint64_t pmm_usable_page_count(void) { return usable_pages; }
uint64_t pmm_free_page_count(void) { return free_pages; }

/*
 * Hand a range that was reserved at boot back to the allocator.
 *
 * Only pages that are still exactly as reserve_page() left them go: allocated
 * with no reference. A page with a reference is one pmm_alloc_page() handed
 * out, and freeing that from here would put a live page on the free list.
 */
uint64_t pmm_release_reserved(uint64_t physical, uint64_t length) {
    if (!length) return 0;
    uint64_t first = physical / PMM_PAGE_SIZE;
    uint64_t last = (physical + length + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE;
    if (last > total_pages) last = total_pages;
    uint64_t released = 0;
    for (uint64_t page = first; page < last; page++) {
        if (!bit_test(page) || refcounts[page]) continue;
        bit_clear(page);
        free_pages++;
        released++;
        if (page < next_hint) next_hint = page;
    }
    return released;
}
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
