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

/* The seals F_ADD_SEALS understands, with Linux's values. */
#define MEMFD_SEAL_SEAL 0x0001U
#define MEMFD_SEAL_SHRINK 0x0002U
#define MEMFD_SEAL_GROW 0x0004U
#define MEMFD_SEAL_WRITE 0x0008U
#define MEMFD_SEAL_ALL (MEMFD_SEAL_SEAL | MEMFD_SEAL_SHRINK | MEMFD_SEAL_GROW | MEMFD_SEAL_WRITE)

struct memfd_object;

struct memfd_object *memfd_create_object(void);
void memfd_destroy(struct memfd_object *object);

uint64_t memfd_size(const struct memfd_object *object);

/*
 * Sealing. It is not decoration: Mesa asks for F_SEAL_SHRINK before it will
 * hand a shared buffer to a compositor, and closes the descriptor and gives up
 * when the request fails -- which is how every OpenGL client on this system
 * came to die with `invalid arguments for wl_shm.create_pool`, having sent a
 * request with no descriptor attached to it.
 */
void memfd_allow_sealing(struct memfd_object *object);
uint32_t memfd_seals(const struct memfd_object *object);
int memfd_add_seals(struct memfd_object *object, uint32_t seals);
/* Grow or shrink to `size` bytes, allocating or releasing whole pages. */
int memfd_truncate(struct memfd_object *object, uint64_t size);
/* Physical address of the page at `index`, or 0 when it is past the end. */
uint64_t memfd_page(const struct memfd_object *object, uint64_t index);

int64_t memfd_read(struct memfd_object *object, uint64_t offset,
                   size_t length, void *out);
int64_t memfd_write(struct memfd_object *object, uint64_t offset,
                    size_t length, const void *in);

#endif
