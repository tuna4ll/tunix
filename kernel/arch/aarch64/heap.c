#include <stddef.h>
#include <stdint.h>

#include "arch.h"

extern char __image_end[];

#define HEAP_SIZE (16UL * 1024 * 1024)
#define ALIGN_UP(x, a) (((x) + (a) - 1) & ~((a) - 1))

struct block {
    uint64_t size;                  // payload bytes
    uint64_t free;
};

static uint64_t heap_start;
static uint64_t heap_end;

void heap_init(void) {
    heap_start = ALIGN_UP((uint64_t)__image_end, 4096);
    heap_end = heap_start + HEAP_SIZE;
    pmm_reserve(heap_start, HEAP_SIZE);

    struct block *first = (struct block *)heap_start;
    first->size = HEAP_SIZE - sizeof(struct block);
    first->free = 1;
}

static struct block *next_block(struct block *b) {
    uint64_t n = (uint64_t)b + sizeof(struct block) + b->size;
    return n < heap_end ? (struct block *)n : NULL;
}

void *kmalloc(size_t want) {
    if (!want) return NULL;
    want = ALIGN_UP(want, 16);
    for (struct block *b = (struct block *)heap_start; b; b = next_block(b)) {
        if (!b->free || b->size < want) continue;
        if (b->size >= want + sizeof(struct block) + 16) {
            struct block *split = (struct block *)((uint64_t)b + sizeof(struct block) + want);
            split->size = b->size - want - sizeof(struct block);
            split->free = 1;
            b->size = want;
        }
        b->free = 0;
        return (void *)((uint64_t)b + sizeof(struct block));
    }
    return NULL;
}

void kfree(void *ptr) {
    if (!ptr) return;
    struct block *b = (struct block *)((uint64_t)ptr - sizeof(struct block));
    b->free = 1;
    // Coalesce every run of adjacent free blocks.
    for (struct block *c = (struct block *)heap_start; c;) {
        struct block *n = next_block(c);
        if (c->free && n && n->free) {
            c->size += sizeof(struct block) + n->size;
            continue;
        }
        c = n;
    }
}

uint64_t heap_free_bytes(void) {
    uint64_t total = 0;
    for (struct block *b = (struct block *)heap_start; b; b = next_block(b))
        if (b->free) total += b->size;
    return total;
}
