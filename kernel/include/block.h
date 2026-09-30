#ifndef TUNIX_BLOCK_H
#define TUNIX_BLOCK_H

#include <stddef.h>
#include <stdint.h>

#define BLOCK_SECTOR_SIZE 512U

#define BLOCK_NAME_BYTES 16

struct block_device {
    char name[16];

    char dev_name[BLOCK_NAME_BYTES];

    int parent;
    uint64_t sectors;

    int (*read)(void *context, uint64_t lba, uint32_t count, void *destination);
    int (*write)(void *context, uint64_t lba, uint32_t count, const void *source);

    int (*flush)(void *context);
    void *context;
};

int block_register(const struct block_device *device);

int block_register_partition(int parent, int number, uint64_t start,
                             uint64_t sectors);
int block_device_count(void);
const struct block_device *block_device_at(int index);

int block_device_index_by_name(const char *name);

void block_select_root(int index);
const struct block_device *block_root(void);

int block_device_read_bytes(const struct block_device *device, uint64_t offset,
                            size_t size, void *destination);
int block_device_write_bytes(const struct block_device *device, uint64_t offset,
                             size_t size, const void *source);
int block_device_read(const struct block_device *device, uint64_t lba,
                      uint32_t count, void *destination);
int block_device_write(const struct block_device *device, uint64_t lba,
                       uint32_t count, const void *source);
int block_device_flush(const struct block_device *device);
int block_read(uint64_t lba, uint32_t count, void *destination);
int block_write(uint64_t lba, uint32_t count, const void *source);
int block_read_bytes(uint64_t offset, size_t size, void *destination);
int block_flush(void);
uint64_t block_sectors(void);

void block_probe(void);

void block_statistics(uint64_t *reads, uint64_t *sectors, uint64_t *nanoseconds,
                      uint64_t *write_failures);

#endif
