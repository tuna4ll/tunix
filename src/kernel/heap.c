#include "include/heap.h"
#include "include/vmm.h"
#include "include/pmm.h"
#include "include/spinlock.h"

#define HEAP_START 0xFFFFFFFFC0000000
#define HEAP_INITIAL_SIZE (1024 * 1024)
/* The heap grows on demand from the PMM, so this ceiling costs nothing until
 * hit; if physical RAM runs out first heap_grow() just fails gracefully. It was
 * 96 MiB, which capped Tunix's in-RAM filesystem (file data lives in kmalloc'd
 * buffers via memory_write) far too low: `git clone` of a real repo writes a
 * multi-MB pack into RAM and, with the capacity-doubling realloc's transient
 * ~2x peak, exhausted the heap and surfaced as EPERM write errors. 768 MiB sits
 * comfortably inside the 1 GiB virtual window above HEAP_START and lets a clone
 * fit when QEMU is given enough RAM (see the run targets' -m). */
#define HEAP_MAX_SIZE (768ULL * 1024 * 1024)
#define HEAP_PAGE_SIZE 4096ULL
#define HEAP_MAGIC 0x1234ABCD

typedef struct heap_block {
    uint32_t magic;
    uint32_t size;
    uint8_t is_free;
    struct heap_block* next;
} heap_block_t;

static heap_block_t* head = NULL;
static uint64_t heap_size = 0;
static spinlock_t heap_lock;

extern void kprintf(const char *fmt, ...);
extern void panic(const char *msg);

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
    head->next = NULL;
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
    new_block->next = NULL;

    heap_block_t *tail = head;
    while (tail->next) tail = tail->next;
    if (tail->is_free &&
        (uint64_t)tail + sizeof(heap_block_t) + tail->size == base) {
        tail->size += (uint32_t)growth;
    } else {
        tail->next = new_block;
    }

    heap_size += growth;
    return 0;
}

void* kmalloc(size_t size) {
    if (size == 0) return NULL;

    spinlock_acquire(&heap_lock);

    for (;;) {
        heap_block_t* curr = head;
        while (curr != NULL) {
            if (curr->is_free && curr->size >= size) {
                if (curr->size > size + sizeof(heap_block_t) + 16) {
                    heap_block_t* new_block = (heap_block_t*)((uint8_t*)curr + sizeof(heap_block_t) + size);
                    new_block->magic = HEAP_MAGIC;
                    new_block->size = curr->size - size - sizeof(heap_block_t);
                    new_block->is_free = 1;
                    new_block->next = curr->next;

                    curr->size = size;
                    curr->next = new_block;
                }

                curr->is_free = 0;
                spinlock_release(&heap_lock);
                return (void*)((uint8_t*)curr + sizeof(heap_block_t));
            }
            curr = curr->next;
        }

        if (heap_grow(size) != 0) {
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

    block->is_free = 1;

    heap_block_t* curr = head;
    while (curr != NULL) {
        if (curr->is_free && curr->next != NULL && curr->next->is_free) {
            curr->size += curr->next->size + sizeof(heap_block_t);
            curr->next = curr->next->next;
        } else {
            curr = curr->next;
        }
    }

    spinlock_release(&heap_lock);
}
