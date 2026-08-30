#include <stddef.h>
#include <stdint.h>
#include "../include/heap.h"
#include "../include/kstring.h"
#include "../include/memfd.h"
#include "../include/pmm.h"
#include "../include/vmm.h"

/*
 * See include/memfd.h for what this is for. The implementation is deliberately
 * plain: a growable array of physical page addresses, one reference held per
 * page. There is no swapping, no sparse representation and no sealing -- a
 * Wayland client sizes its buffer once and keeps it.
 */

/* A cap so a stray ftruncate cannot eat the machine. 256 MiB is far above any
   wl_shm buffer (a 4K screen at 32bpp is 33 MiB) and far below total RAM. */
#define MEMFD_MAX_BYTES (256ULL * 1024ULL * 1024ULL)
#define MEMFD_PAGE_SIZE 4096ULL

struct memfd_object {
    uint64_t size;      /* logical length, as set by ftruncate */
    /* Sealing, as F_ADD_SEALS applies it. `sealable` is what
       MFD_ALLOW_SEALING asked for; without it the file refuses every seal,
       which is what Linux does and what a caller checks for. */
    uint32_t seals;
    int sealable;
    uint64_t count;     /* pages actually allocated */
    uint64_t capacity;  /* entries available in `pages` */
    uint64_t *pages;    /* physical addresses */
};

static uint64_t pages_for(uint64_t size) {
    return (size + MEMFD_PAGE_SIZE - 1ULL) / MEMFD_PAGE_SIZE;
}

struct memfd_object *memfd_create_object(void) {
    struct memfd_object *object = (struct memfd_object *)kmalloc(sizeof(*object));
    if (!object) return NULL;
    memset(object, 0, sizeof(*object));
    return object;
}

void memfd_destroy(struct memfd_object *object) {
    if (!object) return;
    /* Drops this object's reference to each page; any page still mapped
       somewhere survives on the mapping's own reference. */
    for (uint64_t index = 0; index < object->count; index++) {
        if (object->pages[index]) pmm_free_page((void *)object->pages[index]);
    }
    kfree(object->pages);
    kfree(object);
}

void memfd_allow_sealing(struct memfd_object *object) {
    if (object) object->sealable = 1;
}

uint32_t memfd_seals(const struct memfd_object *object) {
    return object ? object->seals : 0;
}

/*
 * Add seals, with the refusals Linux makes.
 *
 * Returns 0, or the negative errno a caller expects: -EINVAL when the file was
 * not created sealable or the bits are not seals, -EPERM once F_SEAL_SEAL has
 * been set and the set is closed.
 */
int memfd_add_seals(struct memfd_object *object, uint32_t seals) {
    if (!object || !object->sealable) return -22;          /* EINVAL */
    if (seals & ~(uint32_t)MEMFD_SEAL_ALL) return -22;
    if (object->seals & MEMFD_SEAL_SEAL) return -1;        /* EPERM */
    object->seals |= seals;
    return 0;
}

uint64_t memfd_size(const struct memfd_object *object) {
    return object ? object->size : 0;
}

uint64_t memfd_page(const struct memfd_object *object, uint64_t index) {
    if (!object || index >= object->count) return 0;
    return object->pages[index];
}

/* The page array only ever grows; shrinking the object frees pages but keeps
   the (small) array, so a buffer that is resized repeatedly does not churn. */
static int reserve_pages(struct memfd_object *object, uint64_t needed) {
    if (needed <= object->capacity) return 0;
    uint64_t capacity = object->capacity ? object->capacity : 16ULL;
    while (capacity < needed) capacity *= 2ULL;
    uint64_t *pages = (uint64_t *)kmalloc(capacity * sizeof(uint64_t));
    if (!pages) return -1;
    memset(pages, 0, capacity * sizeof(uint64_t));
    if (object->pages) {
        memcpy(pages, object->pages, object->count * sizeof(uint64_t));
        kfree(object->pages);
    }
    object->pages = pages;
    object->capacity = capacity;
    return 0;
}

int memfd_truncate(struct memfd_object *object, uint64_t size) {
    if (!object) return -1;
    if (size > MEMFD_MAX_BYTES) return -1;
    /* The two seals that mean what they say here. A shrink-sealed file is the
       promise Mesa wants before it hands a buffer to the compositor: the pages
       under the mapping cannot be taken away while the other side is reading
       them. */
    if (size < object->size && (object->seals & MEMFD_SEAL_SHRINK)) return -1;
    if (size > object->size && (object->seals & MEMFD_SEAL_GROW)) return -1;

    uint64_t wanted = pages_for(size);
    if (wanted > object->count) {
        if (reserve_pages(object, wanted) != 0) return -1;
        while (object->count < wanted) {
            uint64_t physical = (uint64_t)pmm_alloc_page();
            if (!physical) return -1;
            /* Zeroed like any fresh anonymous memory: the consumer of a
               freshly sized buffer must not see another process's leftovers. */
            memset(vmm_phys_to_virt(physical), 0, MEMFD_PAGE_SIZE);
            object->pages[object->count++] = physical;
        }
    } else {
        while (object->count > wanted) {
            uint64_t physical = object->pages[--object->count];
            object->pages[object->count] = 0;
            if (physical) pmm_free_page((void *)physical);
        }
    }
    object->size = size;
    return 0;
}

/*
 * read/write exist so the descriptor behaves like a file for anything that is
 * not mapping it. Wayland itself only ever ftruncates and mmaps, but a
 * descriptor that returns EINVAL to read() is a trap for everything else.
 */
static int64_t transfer(struct memfd_object *object, uint64_t offset,
                        size_t length, void *out, const void *in) {
    if (!object || offset >= object->size) return 0;
    uint64_t remaining = object->size - offset;
    if (length > remaining) length = (size_t)remaining;

    size_t moved = 0;
    while (moved < length) {
        uint64_t index = (offset + moved) / MEMFD_PAGE_SIZE;
        uint64_t within = (offset + moved) % MEMFD_PAGE_SIZE;
        uint64_t physical = memfd_page(object, index);
        if (!physical) break;
        size_t chunk = (size_t)(MEMFD_PAGE_SIZE - within);
        if (chunk > length - moved) chunk = length - moved;
        uint8_t *page = (uint8_t *)vmm_phys_to_virt(physical) + within;
        if (out) memcpy((uint8_t *)out + moved, page, chunk);
        else memcpy(page, (const uint8_t *)in + moved, chunk);
        moved += chunk;
    }
    return (int64_t)moved;
}

int64_t memfd_read(struct memfd_object *object, uint64_t offset,
                   size_t length, void *out) {
    return transfer(object, offset, length, out, NULL);
}

int64_t memfd_write(struct memfd_object *object, uint64_t offset,
                    size_t length, const void *in) {
    return transfer(object, offset, length, NULL, in);
}
