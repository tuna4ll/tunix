#include <stddef.h>
#include <stdint.h>
#include <tunix/block.h>
#include <tunix/errno.h>
#include <tunix/heap.h>
#include <tunix/kprintf.h>
#include <tunix/kstring.h>
#include <tunix/lock.h>
#include <tunix/mutex.h>
#include <tunix/pmm.h>
#include <tunix/process.h>
#include <tunix/swap.h>
#include <tunix/time.h>
#include <tunix/vfs.h>
#include <tunix/vmm.h>

#define SWAP_PAGE_SIZE    4096ULL
#define SWAP_SECTORS      (SWAP_PAGE_SIZE / BLOCK_SECTOR_SIZE)
#define SWAP_OFFSET_MASK  ((1ULL << SWAP_OFFSET_BITS) - 1ULL)
#define SWAP_COUNT_MASK   0x7FFFU
#define SWAP_HAS_CACHE    0x8000U
#define SWAP_BAD          0xFFFFU
#define SWAP_COUNT_MAX    0x7FFEU
#define SWAP_MAGIC_OFFSET 4086U
#define SWAP_INFO_OFFSET  1024U
#define SWAP_MIN_PAGES    10ULL
#define SWAP_PATH_BYTES   128U
#define SWAP_POOL_TARGET  256U
#define SWAP_WAIT_NS      (1000ULL * 1000ULL)

#define CACHE_DIRTY   1U
#define CACHE_IO      2U
#define CACHE_READING 4U
#define CACHE_ERROR   8U
#define CACHE_ORPHAN  16U

struct swap_extent {
    uint64_t first_page;
    uint64_t pages;
    uint64_t sector;
};

struct swap_area {
    int active;
    int draining;
    int is_file;
    int priority;
    int block;
    uint64_t pages;
    uint64_t used;
    uint64_t usable;
    uint64_t cursor;
    uint16_t *map;
    struct swap_extent *extents;
    uint32_t extent_count;
    struct vfs_node *node;
    uint64_t *discards;
    uint64_t discard_pending;
    uint64_t discard_cursor;
    char path[SWAP_PATH_BYTES];
};

struct swap_cached {
    uint64_t slot;
    uint64_t physical;
    uint32_t state;
    struct swap_cached *slot_next;
    struct swap_cached *page_next;
    struct swap_cached *older;
    struct swap_cached *newer;
};

static struct swap_area areas[SWAP_AREAS];
static struct lock swap_lock = LOCK_INITIALIZER("swap", LOCK_RANK_SWAP);
static struct mutex swapon_mutex = MUTEX_INITIALIZER("swapon", LOCK_RANK_SWAPON);
static int least_priority = -1;
static unsigned rotation;
static uint64_t total_usable;
static uint64_t total_used;

static struct swap_cached **slot_buckets;
static struct swap_cached **page_buckets;
static uint64_t bucket_mask;
static struct swap_cached *oldest;
static struct swap_cached *newest;
static uint64_t cached_pages;
static struct swap_cached *pool;
static unsigned pool_count;
static const char io_channel;

static void swap_guard_release(int *unused) {
    (void)unused;
    lock_release(&swap_lock);
}

#define SWAP_LOCKED \
    __attribute__((cleanup(swap_guard_release))) int swap_guard = (lock_acquire(&swap_lock), 0)

static uint64_t bucket_of(uint64_t value) {
    return ((value >> 12) * 0x9E3779B97F4A7C15ULL >> 24) & bucket_mask;
}

static uint64_t slot_make(unsigned area, uint64_t offset) {
    return ((uint64_t)area << SWAP_OFFSET_BITS) | offset;
}

static struct swap_area *slot_area(uint64_t slot, uint64_t *offset) {
    uint64_t index = slot >> SWAP_OFFSET_BITS;
    uint64_t at = slot & SWAP_OFFSET_MASK;
    if (index >= SWAP_AREAS || !areas[index].active || !at || at >= areas[index].pages) return NULL;
    if (offset) *offset = at;
    return &areas[index];
}

static struct swap_cached *find_slot(uint64_t slot) {
    if (!slot_buckets) return NULL;
    for (struct swap_cached *entry = slot_buckets[bucket_of(slot << 12)]; entry;
         entry = entry->slot_next)
        if (entry->slot == slot) return entry;
    return NULL;
}

static struct swap_cached *find_page(uint64_t physical) {
    if (!page_buckets) return NULL;
    for (struct swap_cached *entry = page_buckets[bucket_of(physical)]; entry;
         entry = entry->page_next)
        if (entry->physical == physical) return entry;
    return NULL;
}

static void unlink_chain(struct swap_cached **head, struct swap_cached *entry, int by_slot) {
    for (struct swap_cached **link = head; *link;
         link = by_slot ? &(*link)->slot_next : &(*link)->page_next) {
        if (*link != entry) continue;
        *link = by_slot ? entry->slot_next : entry->page_next;
        return;
    }
}

static void lru_unlink(struct swap_cached *entry) {
    if (entry->older) entry->older->newer = entry->newer;
    else oldest = entry->newer;
    if (entry->newer) entry->newer->older = entry->older;
    else newest = entry->older;
    entry->older = entry->newer = NULL;
}

static void lru_append(struct swap_cached *entry) {
    entry->newer = NULL;
    entry->older = newest;
    if (newest) newest->newer = entry;
    else oldest = entry;
    newest = entry;
}

static void cache_insert(struct swap_cached *entry) {
    struct swap_cached **slot_head = &slot_buckets[bucket_of(entry->slot << 12)];
    struct swap_cached **page_head = &page_buckets[bucket_of(entry->physical)];
    entry->slot_next = *slot_head;
    *slot_head = entry;
    entry->page_next = *page_head;
    *page_head = entry;
    lru_append(entry);
    cached_pages++;
}

static void slot_release(struct swap_area *area, uint64_t offset) {
    area->map[offset] = 0;
    area->used--;
    total_used--;
    if (offset < area->cursor) area->cursor = offset;
    if (area->discards && !(area->discards[offset / 64U] & (1ULL << (offset % 64U)))) {
        area->discards[offset / 64U] |= 1ULL << (offset % 64U);
        area->discard_pending++;
    }
}

static uint32_t collect_discards(struct swap_area *area, uint64_t *offsets, uint32_t room) {
    uint32_t count = 0;
    uint64_t words = (area->pages + 63U) / 64U;
    for (uint64_t step = 0; step < words && count < room && area->discard_pending; step++) {
        uint64_t word = (area->discard_cursor + step) % words;
        while (area->discards[word] && count < room) {
            unsigned bit = (unsigned)__builtin_ctzll(area->discards[word]);
            uint64_t offset = word * 64U + bit;
            area->discards[word] &= ~(1ULL << bit);
            area->discard_pending--;
            if (area->map[offset]) continue;
            area->map[offset] = SWAP_BAD;
            offsets[count++] = offset;
        }
        area->discard_cursor = word;
    }
    return count;
}

void swap_discard_released(void) {
    for (unsigned index = 0; index < SWAP_AREAS; index++) {
        for (unsigned batch = 0; batch < 64U; batch++) {
            uint64_t offsets[256];
            uint32_t count = 0;
            int block = -1;
            {
                SWAP_LOCKED;
                struct swap_area *area = &areas[index];
                if (!area->active || !area->discards || !area->discard_pending) break;
                count = collect_discards(area, offsets, 256U);
                block = area->block;
            }
            const struct block_device *device = block_device_at(block);
            for (uint32_t at = 0; device && at < count;) {
                uint32_t run = 1;
                while (at + run < count && offsets[at + run] == offsets[at] + run && run < 4096U)
                    run++;
                (void)block_device_discard(device, offsets[at] * SWAP_SECTORS,
                                           (uint32_t)(run * SWAP_SECTORS));
                at += run;
            }
            SWAP_LOCKED;
            for (uint32_t at = 0; at < count; at++)
                if (areas[index].map && areas[index].map[offsets[at]] == SWAP_BAD)
                    areas[index].map[offsets[at]] = 0;
            if (!count) break;
        }
    }
}

int swap_block_in_use(int block) {
    SWAP_LOCKED;
    for (unsigned index = 0; index < SWAP_AREAS; index++)
        if ((areas[index].active || areas[index].node) && !areas[index].is_file &&
            areas[index].block == block)
            return 1;
    return 0;
}

static void cache_drop(struct swap_cached *entry) {
    unlink_chain(&slot_buckets[bucket_of(entry->slot << 12)], entry, 1);
    unlink_chain(&page_buckets[bucket_of(entry->physical)], entry, 0);
    lru_unlink(entry);
    cached_pages--;
    uint64_t offset;
    struct swap_area *area = slot_area(entry->slot, &offset);
    if (area) {
        area->map[offset] &= (uint16_t)~SWAP_HAS_CACHE;
        if (!area->map[offset]) slot_release(area, offset);
    }
    pmm_free_page((void *)entry->physical);
    entry->slot_next = pool;
    pool = entry;
    pool_count++;
}

static uint64_t slot_alloc(void) {
    struct swap_area *best = NULL;
    unsigned matches = 0;
    for (unsigned index = 0; index < SWAP_AREAS; index++) {
        struct swap_area *area = &areas[index];
        if (!area->active || area->draining || area->used >= area->usable) continue;
        if (!best || area->priority > best->priority) {
            best = area;
            matches = 1;
        } else if (area->priority == best->priority) {
            matches++;
        }
    }
    if (!best) return 0;
    if (matches > 1) {
        unsigned pick = rotation++ % matches;
        for (unsigned index = 0; index < SWAP_AREAS; index++) {
            struct swap_area *area = &areas[index];
            if (!area->active || area->draining || area->used >= area->usable ||
                area->priority != best->priority)
                continue;
            if (!pick--) {
                best = area;
                break;
            }
        }
    }
    for (uint64_t pass = 0; pass < 2; pass++) {
        uint64_t begin = pass ? 1 : (best->cursor ? best->cursor : 1);
        uint64_t end = pass ? best->cursor : best->pages;
        for (uint64_t offset = begin; offset < end; offset++) {
            if (best->map[offset]) continue;
            best->map[offset] = SWAP_HAS_CACHE | 1U;
            best->used++;
            total_used++;
            best->cursor = offset + 1 < best->pages ? offset + 1 : 1;
            return slot_make((unsigned)(best - areas), offset);
        }
    }
    return 0;
}

int swap_reserve_entries(unsigned wanted) {
    if (wanted > SWAP_POOL_TARGET) wanted = SWAP_POOL_TARGET;
    for (;;) {
        {
            SWAP_LOCKED;
            if (pool_count >= wanted || !slot_buckets) return 0;
        }
        struct swap_cached *entry = (struct swap_cached *)kmalloc(sizeof(*entry));
        if (!entry) return -1;
        memset(entry, 0, sizeof(*entry));
        SWAP_LOCKED;
        entry->slot_next = pool;
        pool = entry;
        pool_count++;
    }
}

static struct swap_cached *pool_take(void) {
    struct swap_cached *entry = pool;
    if (!entry) return NULL;
    pool = entry->slot_next;
    pool_count--;
    memset(entry, 0, sizeof(*entry));
    return entry;
}

uint64_t swap_total_pages(void) {
    SWAP_LOCKED;
    return total_usable;
}

uint64_t swap_free_pages(void) {
    SWAP_LOCKED;
    return total_usable > total_used ? total_usable - total_used : 0;
}

uint64_t swap_cached_pages(void) {
    SWAP_LOCKED;
    return cached_pages;
}

int swap_entry_dup(uint64_t slot) {
    SWAP_LOCKED;
    uint64_t offset;
    struct swap_area *area = slot_area(slot, &offset);
    if (!area) return -1;
    uint16_t value = area->map[offset];
    if (value == SWAP_BAD || !(value & SWAP_COUNT_MASK) ||
        (value & SWAP_COUNT_MASK) >= SWAP_COUNT_MAX)
        return -1;
    area->map[offset] = (uint16_t)(value + 1U);
    return 0;
}

void swap_entry_free(uint64_t slot) {
    SWAP_LOCKED;
    uint64_t offset;
    struct swap_area *area = slot_area(slot, &offset);
    if (!area) return;
    uint16_t value = area->map[offset];
    if (value == SWAP_BAD || !(value & SWAP_COUNT_MASK)) {
        kprintf("SWAP: freeing unused slot %p\n", (void *)slot);
        return;
    }
    value = (uint16_t)(value - 1U);
    area->map[offset] = value;
    if (value & SWAP_COUNT_MASK) return;
    if (!(value & SWAP_HAS_CACHE)) {
        slot_release(area, offset);
        return;
    }
    struct swap_cached *entry = find_slot(slot);
    if (!entry) {
        slot_release(area, offset);
        return;
    }
    if (entry->state & (CACHE_IO | CACHE_READING)) entry->state |= CACHE_ORPHAN;
    else cache_drop(entry);
}

uint64_t swap_cache_take(uint64_t physical) {
    SWAP_LOCKED;
    if (!slot_buckets) return 0;
    struct swap_cached *entry = find_page(physical);
    if (entry) {
        uint64_t offset;
        struct swap_area *area = slot_area(entry->slot, &offset);
        if (!area || area->draining || (entry->state & (CACHE_READING | CACHE_ERROR))) return 0;
        uint16_t value = area->map[offset];
        if ((value & SWAP_COUNT_MASK) >= SWAP_COUNT_MAX) return 0;
        area->map[offset] = (uint16_t)(value + 1U);
        return entry->slot;
    }
    if (!pool) return 0;
    uint64_t slot = slot_alloc();
    if (!slot) return 0;
    if (pmm_page_ref(physical) != 0) {
        uint64_t offset;
        struct swap_area *area = slot_area(slot, &offset);
        if (area) slot_release(area, offset);
        return 0;
    }
    entry = pool_take();
    entry->slot = slot;
    entry->physical = physical;
    entry->state = CACHE_DIRTY;
    cache_insert(entry);
    return slot;
}

int swap_install_decision(uint64_t slot, uint64_t physical, uint64_t *flags) {
    SWAP_LOCKED;
    uint64_t offset;
    struct swap_area *area = slot_area(slot, &offset);
    if (!area) return -1;
    uint16_t value = area->map[offset];
    if (value == SWAP_BAD || !(value & SWAP_COUNT_MASK)) return -1;
    struct swap_cached *entry = find_slot(slot);
    if (!entry || entry->physical != physical || (entry->state & CACHE_READING)) return -1;
    int exclusive = (value & SWAP_COUNT_MASK) == 1U && pmm_page_refcount(physical) == 2U;
    if (exclusive) {
        if (*flags & PAGE_COW) *flags = (*flags & ~PAGE_COW) | PAGE_WRITE;
        return 0;
    }
    if (*flags & PAGE_SHARED) {
        if (*flags & PAGE_WRITE) entry->state |= CACHE_DIRTY;
    } else if (*flags & (PAGE_WRITE | PAGE_COW)) {
        *flags = (*flags & ~PAGE_WRITE) | PAGE_COW;
    }
    return 0;
}

static int slot_sector(uint64_t slot, int *block, uint64_t *sector) {
    SWAP_LOCKED;
    uint64_t offset;
    struct swap_area *area = slot_area(slot, &offset);
    if (!area) return -1;
    *block = area->block;
    if (!area->is_file) {
        *sector = offset * SWAP_SECTORS;
        return 0;
    }
    uint32_t low = 0, high = area->extent_count;
    while (low < high) {
        uint32_t middle = low + (high - low) / 2U;
        const struct swap_extent *extent = &area->extents[middle];
        if (offset < extent->first_page) high = middle;
        else if (offset >= extent->first_page + extent->pages) low = middle + 1U;
        else {
            *sector = extent->sector + (offset - extent->first_page) * SWAP_SECTORS;
            return 0;
        }
    }
    return -1;
}

static int slot_io(uint64_t slot, uint64_t physical, int write) {
    int block = -1;
    uint64_t sector = 0;
    if (slot_sector(slot, &block, &sector) != 0) return -EIO;
    const struct block_device *device = block_device_at(block);
    if (!device) return -EIO;
    void *page = vmm_phys_to_virt(physical);
    int status = write ? block_device_write(device, sector, (uint32_t)SWAP_SECTORS, page)
                       : block_device_read(device, sector, (uint32_t)SWAP_SECTORS, page);
    return status == 0 ? 0 : -EIO;
}

static void wait_for_io(void) {
    process_prepare_wait(&io_channel, time_uptime_ns() + SWAP_WAIT_NS);
    process_wait();
    process_finish_wait();
}

static uint64_t cached_page_for(uint64_t slot, int *busy) {
    SWAP_LOCKED;
    *busy = 0;
    struct swap_cached *entry = find_slot(slot);
    if (!entry) return 0;
    if (entry->state & CACHE_READING) {
        *busy = 1;
        return 0;
    }
    if (entry->state & CACHE_ERROR) return 0;
    if (pmm_page_ref(entry->physical) != 0) return 0;
    return entry->physical;
}

static int start_read(uint64_t slot, uint64_t physical, struct swap_cached **out) {
    SWAP_LOCKED;
    if (find_slot(slot)) return 1;
    uint64_t offset;
    struct swap_area *area = slot_area(slot, &offset);
    if (!area || area->map[offset] == SWAP_BAD || !(area->map[offset] & SWAP_COUNT_MASK)) return 2;
    if (!pool) return 3;
    if (pmm_page_ref(physical) != 0) return 2;
    struct swap_cached *entry = pool_take();
    entry->slot = slot;
    entry->physical = physical;
    entry->state = CACHE_READING;
    area->map[offset] |= SWAP_HAS_CACHE;
    cache_insert(entry);
    *out = entry;
    return 0;
}

static void finish_read(struct swap_cached *entry, int status) {
    {
        SWAP_LOCKED;
        entry->state &= ~CACHE_READING;
        if (status != 0) entry->state |= CACHE_ERROR;
        if (status != 0 || (entry->state & CACHE_ORPHAN)) cache_drop(entry);
    }
    process_wake_all(&io_channel);
}

int swap_fault_in(uint64_t cr3, uint64_t address) {
    address &= ~(SWAP_PAGE_SIZE - 1ULL);
    for (unsigned attempt = 0; attempt < 1000U; attempt++) {
        uint64_t slot = 0, flags = 0;
        if (vmm_swap_lookup(cr3, address, &slot, &flags) != 0)
            return vmm_translate(cr3, address, NULL, NULL) == 0 ? 1 : 0;
        int busy = 0;
        uint64_t physical = cached_page_for(slot, &busy);
        if (busy) {
            wait_for_io();
            continue;
        }
        if (!physical) {
            if (swap_reserve_entries(8U) != 0) return -ENOMEM;
            physical = process_alloc_user_page();
            if (!physical) return -ENOMEM;
            struct swap_cached *entry = NULL;
            int started = start_read(slot, physical, &entry);
            if (started != 0) {
                pmm_free_page((void *)physical);
                continue;
            }
            int status = slot_io(slot, physical, 0);
            finish_read(entry, status);
            if (status != 0) {
                pmm_free_page((void *)physical);
                kprintf("SWAP: read error on slot %p\n", (void *)slot);
                return -EIO;
            }
        }
        if (vmm_swap_install(cr3, address, slot, physical) == 0) return 1;
        pmm_free_page((void *)physical);
    }
    return -EAGAIN;
}

unsigned swap_writeback(unsigned budget) {
    swap_discard_released();
    unsigned freed = 0;
    for (unsigned visited = 0; visited < budget; visited++) {
        uint64_t slot = 0, physical = 0;
        struct swap_cached *entry;
        {
            SWAP_LOCKED;
            entry = oldest;
            while (entry && (entry->state & (CACHE_IO | CACHE_READING | CACHE_ERROR)))
                entry = entry->newer;
            if (!entry) return freed;
            lru_unlink(entry);
            lru_append(entry);
            if (pmm_page_refcount(entry->physical) > 1U) continue;
            if (!(entry->state & CACHE_DIRTY)) {
                cache_drop(entry);
                freed++;
                continue;
            }
            entry->state = (entry->state & ~CACHE_DIRTY) | CACHE_IO;
            slot = entry->slot;
            physical = entry->physical;
        }
        int status = slot_io(slot, physical, 1);
        {
            SWAP_LOCKED;
            entry->state &= ~CACHE_IO;
            if (status != 0) {
                entry->state |= CACHE_DIRTY | CACHE_ERROR;
                static unsigned reported;
                if (reported++ < 8U) kprintf("SWAP: write error on slot %p\n", (void *)slot);
            } else if (entry->state & CACHE_ORPHAN) {
                cache_drop(entry);
            } else if (!(entry->state & CACHE_DIRTY) && pmm_page_refcount(physical) == 1U) {
                cache_drop(entry);
                freed++;
            }
        }
        process_wake_all(&io_channel);
    }
    return freed;
}

static int prepare_cache(void) {
    if (slot_buckets) return 0;
    uint64_t buckets = 1024;
    while (buckets < pmm_usable_page_count() / 8ULL && buckets < (1ULL << 22)) buckets <<= 1;
    struct swap_cached **slots = (struct swap_cached **)kmalloc(buckets * sizeof(*slots));
    struct swap_cached **pages = (struct swap_cached **)kmalloc(buckets * sizeof(*pages));
    if (!slots || !pages) {
        kfree(slots);
        kfree(pages);
        return -ENOMEM;
    }
    memset(slots, 0, buckets * sizeof(*slots));
    memset(pages, 0, buckets * sizeof(*pages));
    SWAP_LOCKED;
    page_buckets = pages;
    bucket_mask = buckets - 1ULL;
    slot_buckets = slots;
    return 0;
}

static int device_busy(int block) {
    const struct block_device *device = block_device_at(block);
    if (!device) return 1;
    const struct block_device *root = block_root();
    for (int index = 0; index < block_device_count(); index++) {
        const struct block_device *other = block_device_at(index);
        if (!other) continue;
        int related = index == block || other->parent == block || device->parent == index;
        if (!related) continue;
        if (other == root) return 1;
        vfs_lock_acquire();
        int mounted = 0;
        for (const struct vfs_mount *entry = vfs_mounts(); entry && !mounted; entry = entry->next)
            mounted = entry->dev_major && entry->dev_major == other->major &&
                entry->dev_minor == other->minor;
        vfs_lock_release();
        if (mounted) return 1;
    }
    return 0;
}

struct swap_header_info {
    uint32_t version;
    uint32_t last_page;
    uint32_t bad_pages;
    uint8_t uuid[16];
    uint8_t label[16];
    uint32_t padding[117];
    uint32_t bad[];
};

static int read_header(struct swap_area *area, uint64_t *last_page, uint32_t **bad,
                       uint32_t *bad_count) {
    uint64_t page = (uint64_t)pmm_alloc_page();
    if (!page) return -ENOMEM;
    int block = area->block;
    uint64_t sector = 0;
    if (area->is_file) {
        if (!area->extent_count || area->extents[0].first_page != 0) {
            pmm_free_page((void *)page);
            return -EINVAL;
        }
        sector = area->extents[0].sector;
    }
    const struct block_device *device = block_device_at(block);
    uint8_t *bytes = (uint8_t *)vmm_phys_to_virt(page);
    if (!device || block_device_read(device, sector, (uint32_t)SWAP_SECTORS, bytes) != 0) {
        pmm_free_page((void *)page);
        return -EIO;
    }
    int status = 0;
    const struct swap_header_info *info =
        (const struct swap_header_info *)(bytes + SWAP_INFO_OFFSET);
    uint32_t bad_room = (uint32_t)((SWAP_MAGIC_OFFSET - SWAP_INFO_OFFSET - sizeof(*info)) / 4U);
    if (memcmp(bytes + SWAP_MAGIC_OFFSET, "SWAPSPACE2", 10) != 0 || info->version != 1U ||
        info->last_page < SWAP_MIN_PAGES - 1ULL || info->bad_pages > bad_room) {
        status = -EINVAL;
    } else {
        *last_page = info->last_page;
        *bad_count = info->bad_pages;
        *bad = NULL;
        if (info->bad_pages) {
            *bad = (uint32_t *)kmalloc(info->bad_pages * sizeof(uint32_t));
            if (!*bad) status = -ENOMEM;
            else memcpy(*bad, info->bad, info->bad_pages * sizeof(uint32_t));
        }
    }
    pmm_free_page((void *)page);
    return status;
}

static int map_file(struct swap_area *area, struct vfs_node *node, uint64_t *pages) {
    struct vfs_block_run *runs = NULL;
    uint32_t count = 0;
    int block = -1;
    uint64_t want = node->length / SWAP_PAGE_SIZE;
    if (want < SWAP_MIN_PAGES) return -EINVAL;
    int status = vfs_map_blocks(node, want, &block, &runs, &count);
    if (status != 0) return status;
    struct swap_extent *extents = (struct swap_extent *)kmalloc(count * sizeof(*extents));
    if (!extents) {
        kfree(runs);
        return -ENOMEM;
    }
    uint64_t covered = 0;
    for (uint32_t index = 0; index < count; index++) {
        if (runs[index].first_page != covered || !runs[index].pages) {
            kfree(runs);
            kfree(extents);
            return -EINVAL;
        }
        extents[index].first_page = runs[index].first_page;
        extents[index].pages = runs[index].pages;
        extents[index].sector = runs[index].sector;
        covered += runs[index].pages;
    }
    kfree(runs);
    if (covered < want) {
        kfree(extents);
        return -EINVAL;
    }
    area->block = block;
    area->extents = extents;
    area->extent_count = count;
    *pages = want;
    return 0;
}

int swap_activate(struct vfs_node *node, const char *path, int flags) {
    if (!node || !path) return -EINVAL;
    if (flags & ~(int)(SWAP_FLAG_PRIO | SWAP_FLAG_PREFER | SWAP_FLAG_DISCARD)) return -EINVAL;
    uint32_t type = node->flags & 0xFFU;
    if (type != VFS_BLOCKDEVICE && type != VFS_FILE) return -EINVAL;
    if (prepare_cache() != 0) return -ENOMEM;
    if (swap_reserve_entries(SWAP_POOL_TARGET / 4U) != 0) return -ENOMEM;

    mutex_lock(&swapon_mutex);
    int status = 0;
    unsigned index = SWAP_AREAS;
    {
        SWAP_LOCKED;
        for (unsigned at = 0; at < SWAP_AREAS; at++) {
            if (areas[at].active && areas[at].node == node) {
                status = -EBUSY;
                break;
            }
            if (!areas[at].active && !areas[at].node && index == SWAP_AREAS) index = at;
        }
        if (!status && index == SWAP_AREAS) status = -EPERM;
    }
    if (status) {
        mutex_unlock(&swapon_mutex);
        return status;
    }

    struct swap_area candidate;
    memset(&candidate, 0, sizeof(candidate));
    uint64_t device_pages = 0;
    if (type == VFS_BLOCKDEVICE) {
        candidate.block = block_device_index_by_name(node->name);
        const struct block_device *device = block_device_at(candidate.block);
        if (!device || !device->write) status = -EINVAL;
        else if (device_busy(candidate.block)) status = -EBUSY;
        else device_pages = device->sectors / SWAP_SECTORS;
    } else {
        candidate.is_file = 1;
        if (node->flags & VFS_SWAPFILE) status = -EBUSY;
        else status = map_file(&candidate, node, &device_pages);
    }

    uint64_t last_page = 0;
    uint32_t *bad = NULL;
    uint32_t bad_count = 0;
    if (!status) status = read_header(&candidate, &last_page, &bad, &bad_count);
    uint64_t pages = last_page + 1ULL;
    if (!status && pages > device_pages) pages = device_pages;
    if (!status && pages > SWAP_OFFSET_MASK) pages = SWAP_OFFSET_MASK;
    if (!status && pages < SWAP_MIN_PAGES) status = -EINVAL;
    uint16_t *map = NULL;
    if (!status) {
        map = (uint16_t *)kmalloc(pages * sizeof(uint16_t));
        if (!map) status = -ENOMEM;
    }
    const struct block_device *target = block_device_at(candidate.block);
    if (!status && !candidate.is_file && target && target->discard) {
        uint64_t words = (pages + 63U) / 64U;
        candidate.discards = (uint64_t *)kmalloc(words * sizeof(uint64_t));
        if (!candidate.discards) status = -ENOMEM;
        else memset(candidate.discards, 0, words * sizeof(uint64_t));
    }
    if (status) {
        kfree(bad);
        kfree(map);
        kfree(candidate.discards);
        kfree(candidate.extents);
        mutex_unlock(&swapon_mutex);
        return status;
    }
    memset(map, 0, pages * sizeof(uint16_t));
    map[0] = SWAP_BAD;
    uint64_t usable = pages - 1ULL;
    for (uint32_t at = 0; at < bad_count; at++) {
        if (!bad[at] || bad[at] >= pages || map[bad[at]] == SWAP_BAD) continue;
        map[bad[at]] = SWAP_BAD;
        usable--;
    }
    kfree(bad);

    candidate.map = map;
    candidate.pages = pages;
    candidate.usable = usable;
    candidate.cursor = 1;
    candidate.node = node;
    strncpy(candidate.path, path, sizeof(candidate.path) - 1);
    vfs_node_ref(node);
    if (candidate.is_file) node->flags |= VFS_SWAPFILE;
    {
        SWAP_LOCKED;
        candidate.priority =
            (flags & (int)SWAP_FLAG_PREFER) ? (flags & (int)SWAP_FLAG_PRIO) : --least_priority;
        candidate.active = 1;
        areas[index] = candidate;
        total_usable += usable;
    }
    mutex_unlock(&swapon_mutex);
    kprintf("SWAP: adding %s, %u KiB, priority %d\n", candidate.path,
            (unsigned)(usable * (SWAP_PAGE_SIZE / 1024ULL)), candidate.priority);
    return 0;
}

static int unuse_area(unsigned index) {
    uint64_t low = slot_make(index, 1);
    uint64_t high = slot_make(index, SWAP_OFFSET_MASK);
    for (unsigned round = 0; round < 64U; round++) {
        {
            SWAP_LOCKED;
            if (!areas[index].used) return 0;
        }
        struct process_memory *memories[64];
        unsigned skip = 0;
        for (;;) {
            unsigned count = process_collect_memories(memories, 64, skip);
            if (!count) break;
            skip += count;
            int failed = 0;
            for (unsigned at = 0; at < count; at++) {
                struct process_memory *memory = memories[at];
                if (!failed) {
                    mutex_lock(&memory->lock);
                    uint64_t address = 0;
                    while (!failed && vmm_find_swap(memory->cr3, low, high, &address) == 0) {
                        if (swap_fault_in(memory->cr3, address) < 0) failed = 1;
                        address += SWAP_PAGE_SIZE;
                    }
                    mutex_unlock(&memory->lock);
                }
                process_memory_put(memory);
            }
            if (failed) return -ENOMEM;
        }
        (void)swap_writeback(1U << 20);
    }
    SWAP_LOCKED;
    return areas[index].used ? -EBUSY : 0;
}

int swap_deactivate(struct vfs_node *node) {
    if (!node) return -EINVAL;
    mutex_lock(&swapon_mutex);
    unsigned index = SWAP_AREAS;
    uint64_t usable = 0;
    {
        SWAP_LOCKED;
        for (unsigned at = 0; at < SWAP_AREAS; at++)
            if (areas[at].active && areas[at].node == node) index = at;
        if (index < SWAP_AREAS) {
            usable = areas[index].usable;
            areas[index].draining = 1;
            total_usable -= usable;
        }
    }
    if (index == SWAP_AREAS) {
        mutex_unlock(&swapon_mutex);
        return -EINVAL;
    }
    int status = unuse_area(index);
    struct swap_area released;
    memset(&released, 0, sizeof(released));
    {
        SWAP_LOCKED;
        if (status) {
            areas[index].draining = 0;
            total_usable += usable;
        } else {
            released = areas[index];
            memset(&areas[index], 0, sizeof(areas[index]));
            if (released.priority < 0 && released.priority == least_priority) least_priority++;
        }
    }
    mutex_unlock(&swapon_mutex);
    if (status) return status;
    if (released.is_file) released.node->flags &= ~VFS_SWAPFILE;
    vfs_node_unref(released.node);
    kfree(released.map);
    kfree(released.extents);
    kfree(released.discards);
    kprintf("SWAP: removed %s\n", released.path);
    return 0;
}

void swap_report(swap_report_fn report, void *context) {
    struct swap_area copy;
    for (unsigned index = 0; index < SWAP_AREAS; index++) {
        {
            SWAP_LOCKED;
            if (!areas[index].active) continue;
            copy = areas[index];
        }
        report(context, copy.path, copy.is_file, copy.usable * (SWAP_PAGE_SIZE / 1024ULL),
               copy.used * (SWAP_PAGE_SIZE / 1024ULL), copy.priority);
    }
}
