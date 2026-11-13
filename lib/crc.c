#include <stddef.h>
#include <stdint.h>
#include <tunix/crc.h>

static uint32_t crc32c_table[8][256];
static uint32_t crc32_be_table[256];
static uint16_t crc16_table[256];
static volatile int tables_ready;

static void build_tables(void) {
    for (uint32_t byte = 0; byte < 256U; byte++) {
        uint32_t value = byte;
        for (int bit = 0; bit < 8; bit++)
            value = (value >> 1) ^ (0x82F63B78U & (0U - (value & 1U)));
        crc32c_table[0][byte] = value;

        uint32_t be = byte << 24;
        for (int bit = 0; bit < 8; bit++) be = (be << 1) ^ (0x04C11DB7U & (0U - (be >> 31)));
        crc32_be_table[byte] = be;

        uint16_t small = (uint16_t)byte;
        for (int bit = 0; bit < 8; bit++)
            small = (uint16_t)((small >> 1) ^ (0xA001U & (0U - (small & 1U))));
        crc16_table[byte] = small;
    }
    for (uint32_t byte = 0; byte < 256U; byte++) {
        uint32_t value = crc32c_table[0][byte];
        for (int slice = 1; slice < 8; slice++) {
            value = (value >> 8) ^ crc32c_table[0][value & 0xFFU];
            crc32c_table[slice][byte] = value;
        }
    }
    __atomic_store_n(&tables_ready, 1, __ATOMIC_RELEASE);
}

static void ensure_tables(void) {
    if (!__atomic_load_n(&tables_ready, __ATOMIC_ACQUIRE)) build_tables();
}

uint32_t crc32c(uint32_t crc, const void *data, size_t length) {
    ensure_tables();
    const uint8_t *bytes = (const uint8_t *)data;
    while (length && ((uintptr_t)bytes & 7U)) {
        crc = (crc >> 8) ^ crc32c_table[0][(crc ^ *bytes++) & 0xFFU];
        length--;
    }
    while (length >= 8U) {
        uint32_t low = crc ^
            ((uint32_t)bytes[0] | (uint32_t)bytes[1] << 8 | (uint32_t)bytes[2] << 16 |
             (uint32_t)bytes[3] << 24);
        uint32_t high = (uint32_t)bytes[4] | (uint32_t)bytes[5] << 8 | (uint32_t)bytes[6] << 16 |
            (uint32_t)bytes[7] << 24;
        crc = crc32c_table[7][low & 0xFFU] ^ crc32c_table[6][(low >> 8) & 0xFFU] ^
            crc32c_table[5][(low >> 16) & 0xFFU] ^ crc32c_table[4][low >> 24] ^
            crc32c_table[3][high & 0xFFU] ^ crc32c_table[2][(high >> 8) & 0xFFU] ^
            crc32c_table[1][(high >> 16) & 0xFFU] ^ crc32c_table[0][high >> 24];
        bytes += 8;
        length -= 8;
    }
    while (length--) crc = (crc >> 8) ^ crc32c_table[0][(crc ^ *bytes++) & 0xFFU];
    return crc;
}

uint32_t crc32_be(uint32_t crc, const void *data, size_t length) {
    ensure_tables();
    const uint8_t *bytes = (const uint8_t *)data;
    while (length--) crc = (crc << 8) ^ crc32_be_table[((crc >> 24) ^ *bytes++) & 0xFFU];
    return crc;
}

uint16_t crc16(uint16_t crc, const void *data, size_t length) {
    ensure_tables();
    const uint8_t *bytes = (const uint8_t *)data;
    while (length--) crc = (uint16_t)((crc >> 8) ^ crc16_table[(crc ^ *bytes++) & 0xFFU]);
    return crc;
}
