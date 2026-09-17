#include "include/heap.h"
#include "include/vmm.h"
#include "include/pmm.h"
#include "include/spinlock.h"

#define HEAP_START HEAP_VIRTUAL_BASE
#define HEAP_INITIAL_SIZE (1024 * 1024)
static uint64_t heap_extent_limit(void) {
    static uint64_t limit;
    if (limit) return limit;
    uint64_t ram = pmm_usable_page_count() * (uint64_t)PMM_PAGE_SIZE;
    limit = ram * 2ULL;
    if (limit < 2048ULL * 1024 * 1024) limit = 2048ULL * 1024 * 1024;
    return limit;
}

static uint64_t heap_pressure_size(void) {
    return pmm_usable_page_count() * (uint64_t)PMM_PAGE_SIZE / 4ULL * 3ULL;
}
#define HEAP_FREE_PAGES_FLOOR (16384ULL)
#define HEAP_PAGE_SIZE 4096ULL
#define HEAP_RELEASE_MIN (1024ULL * 1024ULL)
#define HEAP_PAGE_ALIGN_MIN (64ULL * 1024)
#define HEAP_MAGIC 0x1234ABCD

#define HEAP_ALIGN 16ULL

typedef struct heap_block {
    uint32_t magic;
    uint32_t size;
    uint8_t is_free;
    uint8_t pages_released;
    struct heap_block* next;
    struct heap_block* prev;
    struct heap_block* free_prev;
    struct heap_block* free_next;
} heap_block_t;

_Static_assert(sizeof(heap_block_t) % HEAP_ALIGN == 0,
               "the heap header must not disturb the alignment of what follows it");

static heap_block_t* head = NULL;
static heap_block_t* tail = NULL;
static heap_block_t* free_head = NULL;
static heap_block_t* free_tail = NULL;
static uint64_t heap_size = 0;
static uint64_t heap_allocated = 0;
static spinlock_t heap_lock;

extern void kprintf(const char *fmt, ...);
extern void panic(const char *msg);

static void free_insert_after(heap_block_t *anchor, heap_block_t *block) {
    block->free_prev = anchor;
    block->free_next = anchor ? anchor->free_next : free_head;
    if (block->free_next) block->free_next->free_prev = block;
    else free_tail = block;
    if (anchor) anchor->free_next = block;
    else free_head = block;
}

static void free_remove(heap_block_t *block) {
    if (block->free_prev) block->free_prev->free_next = block->free_next;
    else free_head = block->free_next;
    if (block->free_next) block->free_next->free_prev = block->free_prev;
    else free_tail = block->free_prev;
    block->free_prev = NULL;
    block->free_next = NULL;
}

static void free_replace(heap_block_t *old, heap_block_t *block) {
    block->free_prev = old->free_prev;
    block->free_next = old->free_next;
    if (block->free_prev) block->free_prev->free_next = block;
    else free_head = block;
    if (block->free_next) block->free_next->free_prev = block;
    else free_tail = block;
    old->free_prev = NULL;
    old->free_next = NULL;
}

static void free_insert(heap_block_t *block) {
    heap_block_t *left = block->prev;
    heap_block_t *right = block->next;
    while (left || right) {
        if (left) {
            if (left->is_free) { free_insert_after(left, block); return; }
            left = left->prev;
        }
        if (right) {
            if (right->is_free) { free_insert_after(right->free_prev, block); return; }
            right = right->next;
        }
    }
    free_insert_after(NULL, block);
}

void heap_init(void) {
    spinlock_init(&heap_lock);

    for (uint64_t i = 0; i < HEAP_INITIAL_SIZE; i += HEAP_PAGE_SIZE) {
        void* phys = pmm_alloc_page();
        if (!phys) panic("HEAP: PMM out of memory!");
        vmm_map_page(HEAP_START + i, (uint64_t)phys, PAGE_PRESENT | PAGE_WRITE);
    }

    heap_size = HEAP_INITIAL_SIZE;
    head = (heap_block_t*)HEAP_START;
    head->magic = HEAP_MAGIC;
    head->size = HEAP_INITIAL_SIZE - sizeof(heap_block_t);
    head->is_free = 1;
    head->pages_released = 0;
    head->next = NULL;
    head->prev = NULL;
    head->free_prev = NULL;
    head->free_next = NULL;
    tail = head;
    free_head = head;
    free_tail = head;
}

static int heap_grow(size_t min_size) {
    uint64_t needed = (uint64_t)min_size + sizeof(heap_block_t);
    uint64_t growth = (needed + HEAP_PAGE_SIZE - 1) & ~(HEAP_PAGE_SIZE - 1);

    uint64_t ceiling = heap_extent_limit();
    if (heap_size >= ceiling || growth > ceiling - heap_size)
        return -1;

    uint64_t base = HEAP_START + heap_size;
    uint64_t cr3 = vmm_kernel_cr3();
    uint64_t mapped = 0;

    for (; mapped < growth; mapped += HEAP_PAGE_SIZE) {
        void *phys = pmm_alloc_page();
        if (!phys) break;
        if (vmm_map_page_in(cr3, base + mapped, (uint64_t)phys,
                            PAGE_PRESENT | PAGE_WRITE) != 0) {
            pmm_free_page(phys);
            break;
        }
    }

    if (mapped < growth) {
        for (uint64_t i = 0; i < mapped; i += HEAP_PAGE_SIZE) {
            uint64_t physical = 0;
            if (vmm_translate(cr3, base + i, &physical, NULL) == 0) {
                vmm_unmap_page_in(cr3, base + i);
                pmm_free_page((void *)physical);
            }
        }
        return -1;
    }

    heap_block_t *new_block = (heap_block_t *)base;
    new_block->magic = HEAP_MAGIC;
    new_block->size = (uint32_t)(growth - sizeof(heap_block_t));
    new_block->is_free = 1;
    new_block->pages_released = 0;
    new_block->next = NULL;
    new_block->prev = tail;

    if (tail->is_free &&
        (uint64_t)tail + sizeof(heap_block_t) + tail->size == base) {
        tail->size += (uint32_t)growth;
    } else {
        tail->next = new_block;
        tail = new_block;
        free_insert_after(free_tail, new_block);
    }

    heap_size += growth;
    return 0;
}

static void heap_release_pages(heap_block_t *block) {
    if (block->pages_released || block->size < HEAP_RELEASE_MIN) return;

    uint64_t payload = (uint64_t)block + sizeof(heap_block_t);
    uint64_t end = payload + block->size;
    uint64_t first = (payload + HEAP_PAGE_SIZE - 1) & ~(HEAP_PAGE_SIZE - 1);
    uint64_t last = end & ~(HEAP_PAGE_SIZE - 1);
    if (last <= first) return;

    uint64_t cr3 = vmm_kernel_cr3();
    for (uint64_t page = first; page < last; page += HEAP_PAGE_SIZE) {
        uint64_t physical = 0;
        if (vmm_translate(cr3, page, &physical, NULL) != 0) continue;
        if (vmm_unmap_page_in(cr3, page) != 0) continue;
        pmm_free_page((void *)physical);
    }
    block->pages_released = 1;
}

static int heap_reacquire_pages(heap_block_t *block, uint64_t size) {
    if (!block->pages_released) return 0;

    uint64_t payload = (uint64_t)block + sizeof(heap_block_t);
    uint64_t end = payload + block->size;
    uint64_t wanted = payload + size + 2 * HEAP_PAGE_SIZE + sizeof(heap_block_t) + HEAP_ALIGN;
    if (wanted > end) wanted = end;

    uint64_t first = payload & ~(HEAP_PAGE_SIZE - 1);
    uint64_t last = (wanted + HEAP_PAGE_SIZE - 1) & ~(HEAP_PAGE_SIZE - 1);
    uint64_t cr3 = vmm_kernel_cr3();

    for (uint64_t page = first; page < last; page += HEAP_PAGE_SIZE) {
        if (vmm_translate(cr3, page, NULL, NULL) == 0) continue;
        void *physical = pmm_alloc_page();
        if (!physical) return -1;
        if (vmm_map_page_in(cr3, page, (uint64_t)physical,
                            PAGE_PRESENT | PAGE_WRITE) != 0) {
            pmm_free_page(physical);
            return -1;
        }
    }
    return 0;
}

static heap_block_t *split_for_page_alignment(heap_block_t *block, uint64_t size) {
    uint64_t payload = (uint64_t)block + sizeof(heap_block_t);
    if ((payload & (HEAP_PAGE_SIZE - 1)) == 0) return block;
    uint64_t aligned = (payload + HEAP_PAGE_SIZE - 1) & ~(HEAP_PAGE_SIZE - 1);
    while (aligned - payload < sizeof(heap_block_t) + HEAP_ALIGN)
        aligned += HEAP_PAGE_SIZE;
    uint64_t lead = aligned - payload;
    if (block->size < lead + size) return NULL;

    heap_block_t *carved = (heap_block_t *)(aligned - sizeof(heap_block_t));
    carved->magic = HEAP_MAGIC;
    carved->size = (uint32_t)(block->size - lead);
    carved->is_free = 1;
    carved->next = block->next;
    carved->prev = block;
    free_insert_after(block, carved);
    if (carved->next) carved->next->prev = carved;
    else tail = carved;
    block->size = (uint32_t)(lead - sizeof(heap_block_t));
    block->next = carved;
    return carved;
}

void* kmalloc(size_t size) {
    if (size == 0) return NULL;
    size = (size + (HEAP_ALIGN - 1)) & ~(HEAP_ALIGN - 1);
    int page_aligned = size >= HEAP_PAGE_ALIGN_MIN;

    spinlock_acquire(&heap_lock);

    for (;;) {
        heap_block_t* next_free;
        for (heap_block_t* curr = free_head; curr != NULL; curr = next_free) {
            next_free = curr->free_next;
            if (curr->size < size) continue;
            if (heap_reacquire_pages(curr, size) != 0) continue;
            uint8_t released = curr->pages_released;

            heap_block_t* chosen = curr;
            if (page_aligned) {
                chosen = split_for_page_alignment(curr, size);
                if (!chosen) continue;
                curr->pages_released = 0;
                chosen->pages_released = released;
            }
            if (chosen->size > size + sizeof(heap_block_t) + 16) {
                heap_block_t* new_block = (heap_block_t*)((uint8_t*)chosen + sizeof(heap_block_t) + size);
                new_block->magic = HEAP_MAGIC;
                new_block->size = chosen->size - size - sizeof(heap_block_t);
                new_block->is_free = 1;
                new_block->pages_released = released;
                new_block->next = chosen->next;
                new_block->prev = chosen;
                if (new_block->next) new_block->next->prev = new_block;
                else tail = new_block;

                chosen->size = size;
                chosen->next = new_block;
                free_replace(chosen, new_block);
            } else {
                free_remove(chosen);
            }

            chosen->pages_released = 0;
            chosen->is_free = 0;
            heap_allocated += chosen->size;
            spinlock_release(&heap_lock);
            return (void*)((uint8_t*)chosen + sizeof(heap_block_t));
        }

        if (heap_grow(page_aligned ? size + HEAP_PAGE_SIZE + sizeof(heap_block_t)
                                   : size) != 0) {
            spinlock_release(&heap_lock);
            return NULL;
        }
    }
}

void kfree(void* ptr) {
    if (!ptr) return;

    spinlock_acquire(&heap_lock);

    heap_block_t* block = (heap_block_t*)((uint8_t*)ptr - sizeof(heap_block_t));
    if (block->magic != HEAP_MAGIC) {
        spinlock_release(&heap_lock);
        panic("HEAP: Invalid kfree magic!");
    }

    if (block->is_free) {
        spinlock_release(&heap_lock);
        return;
    }
    heap_allocated -= block->size;
    block->is_free = 1;
    heap_release_pages(block);

    int listed = 0;
    heap_block_t* next = block->next;
    if (next && next->is_free) {
        block->size += next->size + sizeof(heap_block_t);
        block->pages_released |= next->pages_released;
        block->next = next->next;
        if (block->next) block->next->prev = block;
        else tail = block;
        free_replace(next, block);
        listed = 1;
    }
    heap_block_t* previous = block->prev;
    if (previous && previous->is_free) {
        previous->size += block->size + sizeof(heap_block_t);
        previous->pages_released |= block->pages_released;
        previous->next = block->next;
        if (previous->next) previous->next->prev = previous;
        else tail = previous;
        if (listed) free_remove(block);
        listed = 1;
    }
    if (!listed) free_insert(block);

    spinlock_release(&heap_lock);
}

int heap_under_pressure(void) {
    spinlock_acquire(&heap_lock);
    int pressed = heap_allocated >= heap_pressure_size();
    spinlock_release(&heap_lock);
    return pressed || pmm_free_page_count() < HEAP_FREE_PAGES_FLOOR;
}

void heap_stats(uint64_t *reserved, uint64_t *allocated, uint64_t *limit) {
    spinlock_acquire(&heap_lock);
    if (reserved) *reserved = heap_size;
    if (allocated) *allocated = heap_allocated;
    spinlock_release(&heap_lock);
    if (limit) *limit = heap_extent_limit();
}
