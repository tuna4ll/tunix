#ifndef TUNIX_MEMFD_H
#define TUNIX_MEMFD_H

#include <stddef.h>
#include <stdint.h>

#define MEMFD_SEAL_SEAL 0x0001U
#define MEMFD_SEAL_SHRINK 0x0002U
#define MEMFD_SEAL_GROW 0x0004U
#define MEMFD_SEAL_WRITE 0x0008U
#define MEMFD_SEAL_FUTURE_WRITE 0x0010U
#define MEMFD_SEAL_EXEC 0x0020U
#define MEMFD_SEAL_ALL (MEMFD_SEAL_SEAL | MEMFD_SEAL_SHRINK | MEMFD_SEAL_GROW | \
                        MEMFD_SEAL_WRITE | MEMFD_SEAL_FUTURE_WRITE | MEMFD_SEAL_EXEC)

struct memfd_object;

struct memfd_object *memfd_create_object(void);
void memfd_ref(struct memfd_object *object);
void memfd_destroy(struct memfd_object *object);

uint64_t memfd_size(const struct memfd_object *object);

void memfd_allow_sealing(struct memfd_object *object);
uint32_t memfd_seals(const struct memfd_object *object);
int memfd_add_seals(struct memfd_object *object, uint32_t seals);
int memfd_truncate(struct memfd_object *object, uint64_t size);
uint64_t memfd_page(const struct memfd_object *object, uint64_t index);

int64_t memfd_read(struct memfd_object *object, uint64_t offset,
                   size_t length, void *out);
int64_t memfd_write(struct memfd_object *object, uint64_t offset,
                    size_t length, const void *in);

#endif
