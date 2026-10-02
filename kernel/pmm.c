#include "include/build_config.h"
#include <stddef.h>
#include <stdint.h>
#include "include/lock.h"

static struct lock pages_lock = LOCK_INITIALIZER("pages", LOCK_RANK_PAGES);
static void *pmm_alloc_page_locked(void);
static void pmm_free_page_locked(void *physical_address);
#include "include/pmm.h"
#include "include/vmm.h"

extern void kprintf(const char *fmt, ...);

#if TUNIX_DEBUG_LOGS
#define KDEBUG(...) kprintf(__VA_ARGS__)
#else
#define KDEBUG(...) do { } while (0)
#endif
extern void panic(const char *msg);

static uint8_t *bitmap;
static uint32_t *refcounts;
static uint64_t total_pages;
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

#define PMM_LOW_MEMORY_RESERVE 0x100000ULL

static uint64_t tracking_physical;
static uint64_t tracking_bytes;
static uint64_t refcount_offset;

static void point_tracking(uint64_t virtual_base) {
    bitmap = (uint8_t *)(virtual_base + tracking_physical);
    refcounts = (uint32_t *)(virtual_base + tracking_physical + refcount_offset);
}

static uint64_t place_tracking(const struct boot_memory_region *regions, uint32_t count) {
    uint64_t best = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (!regions[i].usable) continue;
        uint64_t start = (regions[i].base + PMM_PAGE_SIZE - 1) & ~(PMM_PAGE_SIZE - 1);
        uint64_t end = (regions[i].base + regions[i].length) & ~(PMM_PAGE_SIZE - 1);
        if (start < PMM_LOW_MEMORY_RESERVE) start = PMM_LOW_MEMORY_RESERVE;
        if (end > total_pages * PMM_PAGE_SIZE) end = total_pages * PMM_PAGE_SIZE;
        if (end <= start || end - start < tracking_bytes) continue;
        if (end - tracking_bytes > best) best = end - tracking_bytes;
    }
    return best;
}

void pmm_init(const struct boot_memory_region *regions, uint32_t count) {
    uint64_t highest = 0;
    uint64_t usable = 0;
    uint64_t ignored = 0;

    for (uint32_t i = 0; i < count; i++) {
        uint64_t end = regions[i].base + regions[i].length;
        if (end < regions[i].base) continue;
        if (end > DIRECT_MAP_BYTES) {
            if (regions[i].usable)
                ignored += end - (regions[i].base > DIRECT_MAP_BYTES ? regions[i].base : DIRECT_MAP_BYTES);
            end = DIRECT_MAP_BYTES;
        }
        if (end <= regions[i].base) continue;
        if (end > highest) highest = end;
        if (regions[i].usable) usable += end - regions[i].base;
    }
    if (usable < 2 * 1024 * 1024ULL) panic("PMM: insufficient usable memory");

    total_pages = (highest + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE;
    uint64_t bitmap_bytes = (total_pages + 7) / 8;
    refcount_offset = (bitmap_bytes + 63ULL) & ~63ULL;
    tracking_bytes = (refcount_offset + total_pages * sizeof(uint32_t) + PMM_PAGE_SIZE - 1) &
                     ~(PMM_PAGE_SIZE - 1);
    tracking_physical = place_tracking(regions, count);
    if (!tracking_physical) panic("PMM: no room for page tracking");
    point_tracking(boot_info()->hhdm_offset);

    for (uint64_t i = 0; i < bitmap_bytes; i++) bitmap[i] = 0xFF;
    for (uint64_t i = 0; i < total_pages; i++) refcounts[i] = 0;
    free_pages = 0;

    for (uint32_t i = 0; i < count; i++) {
        if (!regions[i].usable) continue;
        uint64_t start = (regions[i].base + PMM_PAGE_SIZE - 1) & ~(PMM_PAGE_SIZE - 1);
        uint64_t end = (regions[i].base + regions[i].length) & ~(PMM_PAGE_SIZE - 1);
        if (end > highest) end = highest & ~(PMM_PAGE_SIZE - 1);
        for (uint64_t address = start; address < end; address += PMM_PAGE_SIZE) {
            uint64_t page = address / PMM_PAGE_SIZE;
            if (page < total_pages && bit_test(page)) {
                bit_clear(page);
                free_pages++;
            }
        }
    }

    usable_pages = free_pages;

    for (uint64_t page = 0; page < PMM_LOW_MEMORY_RESERVE / PMM_PAGE_SIZE; page++)
        reserve_page(page);
    for (uint64_t page = tracking_physical / PMM_PAGE_SIZE;
         page < (tracking_physical + tracking_bytes) / PMM_PAGE_SIZE; page++) {
        reserve_page(page);
        refcounts[page] = 1;
    }

    next_hint = PMM_LOW_MEMORY_RESERVE / PMM_PAGE_SIZE;

    kprintf("PMM: %u MiB usable, %u KiB of page tracking at %p\n",
            (unsigned)(usable / (1024 * 1024ULL)), (unsigned)(tracking_bytes / 1024ULL),
            (void *)tracking_physical);
    if (ignored)
        kprintf("PMM: %u MiB above the direct map window ignored\n",
                (unsigned)(ignored / (1024 * 1024ULL)));
}

void pmm_use_direct_map(uint64_t virtual_base) {
    lock_acquire(&pages_lock);
    point_tracking(virtual_base);
    lock_release(&pages_lock);
}

void *pmm_alloc_page(void) {
    lock_acquire(&pages_lock);
    void *taken = pmm_alloc_page_locked();
    lock_release(&pages_lock);
    return taken;
}

static void *pmm_alloc_page_locked(void) {
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

void *pmm_alloc_pages_below(uint64_t count, uint64_t alignment_bytes, uint64_t limit) {
    if (!count) return NULL;
    uint64_t stride = alignment_bytes > PMM_PAGE_SIZE
                          ? alignment_bytes / PMM_PAGE_SIZE
                          : 1ULL;
    if (alignment_bytes & (alignment_bytes - 1ULL)) return NULL;
    uint64_t last = limit ? limit / PMM_PAGE_SIZE : total_pages;
    if (last > total_pages) last = total_pages;

    lock_acquire(&pages_lock);
    void *found = NULL;
    if (free_pages >= count) {
        for (uint64_t first = stride; first + count <= last; first += stride) {
            uint64_t page = first;
            while (page < first + count && !bit_test(page)) page++;
            if (page < first + count) {
                first = page - (page % stride);
                continue;
            }
            for (page = first; page < first + count; page++) {
                bit_set(page);
                refcounts[page] = 1;
            }
            free_pages -= count;
            found = (void *)(first * PMM_PAGE_SIZE);
            break;
        }
    }
    lock_release(&pages_lock);
    return found;
}

void *pmm_alloc_pages(uint64_t count, uint64_t alignment_bytes) {
    return pmm_alloc_pages_below(count, alignment_bytes, 0);
}

void pmm_free_pages(void *physical_address, uint64_t count) {
    if (!physical_address || !count) return;
    uint64_t physical = (uint64_t)physical_address;
    lock_acquire(&pages_lock);
    for (uint64_t index = 0; index < count; index++)
        pmm_free_page_locked((void *)(physical + index * PMM_PAGE_SIZE));
    lock_release(&pages_lock);
}

void pmm_free_page(void *physical_address) {
    lock_acquire(&pages_lock);
    pmm_free_page_locked(physical_address);
    lock_release(&pages_lock);
}

static void pmm_free_page_locked(void *physical_address) {
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

int pmm_page_ref(uint64_t physical) {
    lock_acquire(&pages_lock);
    int status = -1;
    uint64_t page = physical / PMM_PAGE_SIZE;
    if (pmm_page_is_allocated(physical) && refcounts[page] != UINT32_MAX) {
        refcounts[page]++;
        status = 0;
    }
    lock_release(&pages_lock);
    return status;
}

uint32_t pmm_page_refcount(uint64_t physical) {
    if (!pmm_page_is_allocated(physical)) return 0;
    return refcounts[physical / PMM_PAGE_SIZE];
}

uint64_t pmm_total_page_count(void) { return total_pages; }
uint64_t pmm_usable_page_count(void) { return usable_pages; }
uint64_t pmm_free_page_count(void) { return free_pages; }

uint64_t pmm_release_reserved(uint64_t physical, uint64_t length) {
    if (!length) return 0;
    uint64_t first = physical / PMM_PAGE_SIZE;
    uint64_t last = (physical + length + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE;
    if (last > total_pages) last = total_pages;
    uint64_t released = 0;
    lock_acquire(&pages_lock);
    for (uint64_t page = first; page < last; page++) {
        if (!bit_test(page) || refcounts[page]) continue;
        bit_clear(page);
        free_pages++;
        released++;
        if (page < next_hint) next_hint = page;
    }
    lock_release(&pages_lock);
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
