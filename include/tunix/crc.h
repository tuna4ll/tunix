#ifndef TUNIX_CRC_H
#define TUNIX_CRC_H

#include <stddef.h>
#include <stdint.h>

uint32_t crc32c(uint32_t crc, const void *data, size_t length);
uint32_t crc32_be(uint32_t crc, const void *data, size_t length);
uint16_t crc16(uint16_t crc, const void *data, size_t length);

#endif
