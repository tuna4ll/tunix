#include "include/heap.h"
#include "include/vmm.h"
#include "include/pmm.h"
#include "include/spinlock.h"

/* Its own PML4 entry (see vmm_init): the slot it used to share with the
   direct map is now direct map, which is where the extra RAM came from. */
#define HEAP_START HEAP_VIRTUAL_BASE
#define HEAP_INITIAL_SIZE (1024 * 1024)
/* The heap grows on demand from the PMM, so this ceiling costs nothing until
 * hit; if physical RAM runs out first heap_grow() just fails gracefully. It was
 * 96 MiB, which capped Tunix's in-RAM filesystem (file data lives in kmalloc'd
 * buffers via memory_write) far too low: `git clone` of a real repo writes a
 * multi-MB pack into RAM and, with the capacity-doubling realloc's transient
 * ~2x peak, exhausted the heap and surfaced as EPERM write errors. 768 MiB sits
 * comfortably inside the 1 GiB virtual window above HEAP_START and lets a clone
 * fit when QEMU is given enough RAM (see the run targets' -m). */
#define HEAP_MAX_SIZE (2048ULL * 1024 * 1024)
/* Where "nearly full" starts. Reclaiming cached file data is not free -- the
   bytes have to be read off the disk again -- so it should not begin the moment
   the heap is merely busy, and it must begin early enough that the allocation
   which actually needs the room still finds it. */
#define HEAP_PRESSURE_SIZE (HEAP_MAX_SIZE / 4 * 3)
/*
 * The other half of "nearly full", and on most machines the half that fires.
 *
 * The fraction above is measured against a ceiling that has nothing to do with
 * how much memory the machine has: on one with 2 GiB the heap would exhaust
 * physical memory long before it reached three quarters of 2 GiB, and reclaim
 * would never start. What actually matters is whether the machine is running
 * out, so that is asked directly. 64 MiB is comfortably more than the largest
 * single thing anything here allocates.
 */
#define HEAP_FREE_PAGES_FLOOR (16384ULL)
#define HEAP_PAGE_SIZE 4096ULL
/* How big a free block has to be before its pages go back to the PMM.
 * Unmapping and remapping costs a page-table walk per page, so doing it for
 * every small allocation would tax the whole system to recover bytes that the
 * next kmalloc reuses anyway. A megabyte is above anything allocated in a hot
 * path and below the file buffers this exists for; smaller blocks still get
 * there by coalescing with their neighbours. */
#define HEAP_RELEASE_MIN (1024ULL * 1024ULL)
/* Allocations this big come back page aligned: file contents land in them and
   mmap maps those pages straight into user processes. */
#define HEAP_PAGE_ALIGN_MIN (64ULL * 1024)
#define HEAP_MAGIC 0x1234ABCD

/*
 * Every allocation comes back 16-byte aligned.
 *
 * That is not a nicety: fxsave64 faults outright on a misaligned operand, and
 * struct process embeds its 512-byte FPU save area. The header used to be 24
 * bytes, so every pointer handed out was block+24 -- 8-aligned at best -- and
 * an `__attribute__((aligned(16)))` member inside a kmalloc'd struct was a
 * promise the allocator could not keep. Padding the header to a multiple of 16
 * and rounding every request up to 16 keeps the invariant by induction from a
 * page-aligned heap base.
 */
#define HEAP_ALIGN 16ULL

typedef struct heap_block {
    uint32_t magic;
    uint32_t size;
    uint8_t is_free;
    /* "Pages inside this block may not be mapped." Deliberately not "are not":
       splitting and coalescing move the boundaries around, so the flag is a
       hint to go and look rather than a record of which pages went. Every
       remap consults the page tables page by page, which makes it idempotent
       and immune to the boundaries having moved. */
    uint8_t pages_released;
    struct heap_block* next;
    /* The list runs both ways so that freeing does not have to find the block
       before this one by walking from the start. It used to, on every kfree,
       through a list that reaches nine thousand blocks on a running desktop --
       and all of it to answer "is my neighbour free too?". */
    struct heap_block* prev;
} heap_block_t;

_Static_assert(sizeof(heap_block_t) % HEAP_ALIGN == 0,
               "the heap header must not disturb the alignment of what follows it");

static heap_block_t* head = NULL;
/* The last block, so growing the heap does not walk to find it. */
static heap_block_t* tail = NULL;
/*
 * A block at or before the first free one.
 *
 * Only a lower bound, which is all a search needs: starting here instead of at
 * head skips the run of allocated blocks that builds up at the bottom of the
 * heap and never comes back. Kept honest by kfree, which is the only thing that
 * can put a free block earlier than this.
 */
static heap_block_t* first_free = NULL;
static uint64_t heap_size = 0;
/* Bytes currently handed out. heap_size only ever grows, so it says nothing
   about how much room is left; this does. */
static uint64_t heap_allocated = 0;
static spinlock_t heap_lock;

extern void kprintf(const char *fmt, ...);
extern void panic(const char *msg);

/*
 * Walk the whole list and stop at the first block that is not one, naming the
 * allocation that ran just before the damage appeared.
 *
 * Off by default: it is O(blocks) on every kmalloc and kfree, and the list
 * reaches thousands of entries on a running system. It exists because the
 * cheap check below can only report corruption when the allocator next walks
 * into it, which may be thousands of operations after the write -- and the
 * whole difficulty of a heap bug is that distance. Build with
 * -DTUNIX_HEAP_DEBUG=1 to close it.
 */
#ifndef TUNIX_HEAP_DEBUG
#define TUNIX_HEAP_DEBUG 0
#endif

#if TUNIX_HEAP_DEBUG
static void heap_validate_all(const char *what, const void *caller,
                              const void *subject, uint64_t size) {
    const heap_block_t *curr = head;
    const heap_block_t *prev = NULL;
    while (curr) {
        uint64_t address = (uint64_t)curr;
        if (address < HEAP_START || address >= HEAP_START + heap_size ||
            curr->magic != HEAP_MAGIC) {
            kprintf("HEAP: damaged after %s(%p, %u) called from %p\n",
                    what, subject, (unsigned)size, caller);
            kprintf("HEAP: bad block %p, previous %p", (void *)curr, (void *)prev);
            if (prev)
                kprintf(" size %u free %u next %p", (unsigned)prev->size,
                        (unsigned)prev->is_free, (void *)prev->next);
            kprintf("\n");
            panic("heap corruption");
        }
        prev = curr;
        curr = curr->next;
    }
}
#define HEAP_VALIDATE(what, subject, size) \
    heap_validate_all((what), __builtin_return_address(0), (subject), (size))
#else
#define HEAP_VALIDATE(what, subject, size) ((void)0)
#endif

/*
 * Stop on a block that is not one, and say who pointed at it.
 *
 * Every block reached by the search below arrives through some earlier
 * block's next pointer, so a damaged pointer or a trampled header is only
 * noticed when it is followed -- by then the address is arbitrary and the
 * fault it takes names the allocator rather than whatever wrote there. This
 * is a load and two compares on a path that already walks the list, and it
 * turns that into the address of the block whose neighbour went wrong, which
 * is close enough to search a suspect's own allocation for.
 */
static void heap_check_block(const heap_block_t *block, const heap_block_t *from) {
    uint64_t address = (uint64_t)block;
    if (address >= HEAP_START && address < HEAP_START + heap_size &&
        block->magic == HEAP_MAGIC)
        return;
    kprintf("HEAP: block %p is not a block", (void *)address);
    if (address >= HEAP_START && address < HEAP_START + heap_size)
        kprintf(" (magic %x)", (unsigned)block->magic);
    else
        kprintf(" (outside the heap, %p..%p)", (void *)HEAP_START,
                (void *)(HEAP_START + heap_size));
    if (from)
        kprintf(", reached from %p size %u free %u next %p prev %p",
                (void *)from, (unsigned)from->size, (unsigned)from->is_free,
                (void *)from->next, (void *)from->prev);
    kprintf("\n");
    panic("heap corruption");
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
    tail = head;
    first_free = head;
}

/* Maps fresh physical pages right after the current end of the heap so
 * kmalloc can satisfy a request no existing free block is big enough
 * for. Returns 0 on success, -1 if physical memory is exhausted or the
 * heap has hit HEAP_MAX_SIZE. Must be called with heap_lock held. */
static int heap_grow(size_t min_size) {
    uint64_t needed = (uint64_t)min_size + sizeof(heap_block_t);
    uint64_t growth = (needed + HEAP_PAGE_SIZE - 1) & ~(HEAP_PAGE_SIZE - 1);

    if (heap_size >= HEAP_MAX_SIZE || growth > HEAP_MAX_SIZE - heap_size)
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

    /* Both outcomes leave free space at or after `tail`, and first_free is
       never past `tail`, so the hint stays a lower bound either way. */
    if (tail->is_free &&
        (uint64_t)tail + sizeof(heap_block_t) + tail->size == base) {
        tail->size += (uint32_t)growth;
    } else {
        tail->next = new_block;
        tail = new_block;
    }

    heap_size += growth;
    return 0;
}

/*
 * Give the pages inside a free block back to the physical allocator.
 *
 * Without this the heap is a one-way street: it takes pages from the PMM and
 * never returns them, so every byte of file content that passes through it
 * permanently stops being memory a process can use. Reading a few hundred
 * megabytes -- which a browser does in minutes -- eventually leaves the machine
 * up and unable to start anything at all.
 *
 * Only whole pages strictly inside the block go. The header must stay mapped
 * because the allocator walks this list by following pointers into headers, and
 * so must anything sharing the header's page or the next block's.
 *
 * Called on a block the moment it is freed and before it coalesces with its
 * neighbours, which is the only point where its extent is exactly the buffer
 * that was allocated: every page inside it is mapped, and every page inside it
 * is now spare. Once blocks merge, "released" is true of the union as soon as
 * it was true of either half, and a block that is mostly mapped would then
 * never be looked at again -- which is precisely how this managed to release
 * nothing at all on its first outing.
 */
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

/*
 * Put pages back under as much of a released block as this allocation will
 * touch, and no more: remapping a 200 MiB block to satisfy a 200-byte request
 * would undo the point of having released it. What the allocator itself is
 * about to write -- the split header just past the payload, and the page the
 * alignment carve may skip over -- is included.
 *
 * Returns 0, or -1 if physical memory has run out. A partial remap is not
 * unwound: the pages it did map are inside a block that is still free and
 * still flagged, so they stay available to the heap and the next attempt
 * simply finds fewer of them missing. Unwinding would have to distinguish the
 * pages this call mapped from the ones that were already there, which the page
 * tables alone cannot say.
 */
static int heap_reacquire_pages(heap_block_t *block, uint64_t size) {
    if (!block->pages_released) return 0;

    uint64_t payload = (uint64_t)block + sizeof(heap_block_t);
    uint64_t end = payload + block->size;
    /* Two pages of slack, not one: the alignment carve can push the payload a
       whole page forward, and then a second if the skipped bytes are too few to
       hold a header of their own. */
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

/* Carve `block` so the payload of the block returned starts on a page boundary,
   leaving the skipped bytes behind as a free block of their own. NULL when this
   block cannot hold the request that way. */
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
    if (carved->next) carved->next->prev = carved;
    else tail = carved;
    block->size = (uint32_t)(lead - sizeof(heap_block_t));
    block->next = carved;
    return carved;
}

void* kmalloc(size_t size) {
    if (size == 0) return NULL;
    /* Round up so the *next* block starts aligned too. */
    size = (size + (HEAP_ALIGN - 1)) & ~(HEAP_ALIGN - 1);
    int page_aligned = size >= HEAP_PAGE_ALIGN_MIN;

    spinlock_acquire(&heap_lock);

    for (;;) {
        heap_block_t* curr = first_free ? first_free : head;
        heap_block_t* previous = NULL;
        while (curr != NULL) {
            heap_check_block(curr, previous);
            if (curr->is_free && curr->size >= size) {
                /* Pages first: everything below writes headers into this block,
                   and a released block has nothing behind those addresses. */
                if (heap_reacquire_pages(curr, size) != 0) { previous = curr; curr = curr->next; continue; }
                uint8_t released = curr->pages_released;

                heap_block_t* chosen = curr;
                if (page_aligned) {
                    chosen = split_for_page_alignment(curr, size);
                    if (!chosen) { previous = curr; curr = curr->next; continue; }
                    curr->pages_released = 0;
                    chosen->pages_released = released;
                }
                if (chosen->size > size + sizeof(heap_block_t) + 16) {
                    heap_block_t* new_block = (heap_block_t*)((uint8_t*)chosen + sizeof(heap_block_t) + size);
                    new_block->magic = HEAP_MAGIC;
                    new_block->size = chosen->size - size - sizeof(heap_block_t);
                    new_block->is_free = 1;
                    /* Only as far as the request needed was mapped, so the
                       remainder is the part that may still be short of pages. */
                    new_block->pages_released = released;
                    new_block->next = chosen->next;
                    new_block->prev = chosen;
                    if (new_block->next) new_block->next->prev = new_block;
                    else tail = new_block;

                    chosen->size = size;
                    chosen->next = new_block;
                }

                /* What is handed out is mapped for its whole length. */
                chosen->pages_released = 0;
                chosen->is_free = 0;
                heap_allocated += chosen->size;
                /*
                 * Move the hint on only when the block it names has stopped
                 * being free, and then only by one. Anything further would
                 * step over the free blocks this search skipped for being too
                 * small -- and the page-alignment carve leaves the block the
                 * hint named still free, which is why this asks rather than
                 * assumes.
                 */
                if (first_free && !first_free->is_free && first_free->next)
                    first_free = first_free->next;
                void *result = (void*)((uint8_t*)chosen + sizeof(heap_block_t));
                HEAP_VALIDATE("kmalloc", result, size);
                spinlock_release(&heap_lock);
                return result;
            }
            previous = curr;
            curr = curr->next;
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

    /* Guarded rather than unconditional: a double free would otherwise take the
       counter below zero and leave the heap permanently claiming to be empty. */
    if (!block->is_free) heap_allocated -= block->size;
    block->is_free = 1;
    heap_release_pages(block);

    /* Both neighbours, and only them: the list is ordered by address and every
       other block was already coalesced with its own neighbours when it was
       freed, so there is nothing further away left to join. */
    heap_block_t* next = block->next;
    if (next && next->is_free) {
        block->size += next->size + sizeof(heap_block_t);
        /* Either side may be short of pages, so the merged block is too. */
        block->pages_released |= next->pages_released;
        block->next = next->next;
        if (block->next) block->next->prev = block;
        else tail = block;
    }
    heap_block_t* previous = block->prev;
    if (previous && previous->is_free) {
        previous->size += block->size + sizeof(heap_block_t);
        previous->pages_released |= block->pages_released;
        previous->next = block->next;
        if (previous->next) previous->next->prev = previous;
        else tail = previous;
        block = previous;
    }

    /* The survivor, which is at or before whatever the hint pointed at -- and
       may be the block the hint pointed at, now absorbed. */
    if (!first_free || block < first_free) first_free = block;

    HEAP_VALIDATE("kfree", ptr, 0);
    spinlock_release(&heap_lock);
}

int heap_under_pressure(void) {
    spinlock_acquire(&heap_lock);
    int pressed = heap_allocated >= HEAP_PRESSURE_SIZE;
    spinlock_release(&heap_lock);
    return pressed || pmm_free_page_count() < HEAP_FREE_PAGES_FLOOR;
}

void heap_stats(uint64_t *reserved, uint64_t *allocated, uint64_t *limit) {
    spinlock_acquire(&heap_lock);
    if (reserved) *reserved = heap_size;
    if (allocated) *allocated = heap_allocated;
    spinlock_release(&heap_lock);
    if (limit) *limit = HEAP_MAX_SIZE;
}
