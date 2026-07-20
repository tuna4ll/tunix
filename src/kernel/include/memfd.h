#ifndef TUNIX_MEMFD_H
#define TUNIX_MEMFD_H

#include <stddef.h>
#include <stdint.h>

/*
 * Anonymous shareable memory, the object behind memfd_create(2).
 *
 * It exists because Wayland has no other way to hand a buffer to a compositor:
 * the client creates one of these, sizes it with ftruncate, maps it MAP_SHARED,
 * and passes the descriptor over a unix socket with SCM_RIGHTS. Both sides then
 * map the *same* physical pages, which is what makes wl_shm work at all.
 *
 * The object owns one reference to each of its pages. Every MAP_SHARED mapping
 * takes another, so closing the descriptor while a mapping is still live leaves
 * the memory alive until the last mapping goes away.
 */

struct memfd_object;

struct memfd_object *memfd_create_object(void);
void memfd_destroy(struct memfd_object *object);

uint64_t memfd_size(const struct memfd_object *object);
/* Grow or shrink to `size` bytes, allocating or releasing whole pages. */
int memfd_truncate(struct memfd_object *object, uint64_t size);
/* Physical address of the page at `index`, or 0 when it is past the end. */
uint64_t memfd_page(const struct memfd_object *object, uint64_t index);

int64_t memfd_read(struct memfd_object *object, uint64_t offset,
                   size_t length, void *out);
int64_t memfd_write(struct memfd_object *object, uint64_t offset,
                    size_t length, const void *in);

#endif
