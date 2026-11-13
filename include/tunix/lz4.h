#ifndef TUNIX_LZ4_H
#define TUNIX_LZ4_H

#include <stddef.h>
#include <stdint.h>

#define TUNIX_LZ4_TABLE_ENTRIES 4096U
#define TUNIX_LZ4_BOUND(length) ((length) + (length) / 255U + 16U)

size_t tunix_lz4_compress(const uint8_t *source, size_t length, uint8_t *output, size_t capacity,
                          uint16_t *table);
int tunix_lz4_decompress(const uint8_t *input, size_t length, uint8_t *output, size_t size);

#endif
