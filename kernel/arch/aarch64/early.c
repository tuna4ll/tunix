#include <stdint.h>

#include "aarch64.h"

#define EARLY_TABLES 40U
#define TABLE_ENTRIES 512U
#define ADDRESS_MASK 0x0000FFFFFFFFF000ULL

#define DESC_VALID (1ULL << 0)
#define DESC_TABLE (3ULL << 0)
#define DESC_PAGE (3ULL << 0)
#define DESC_ATTR_DEVICE (1ULL << 2)
#define DESC_SH_INNER (3ULL << 8)
#define DESC_AF (1ULL << 10)
#define DESC_PXN (1ULL << 53)
#define DESC_UXN (1ULL << 54)

#define BLOCK_2M 0x200000ULL

extern char kernel_image_start[];
extern char __image_end[];

uint64_t boot_root[TABLE_ENTRIES] __attribute__((aligned(4096)));
static uint64_t early_tables[EARLY_TABLES][TABLE_ENTRIES] __attribute__((aligned(4096)));
static unsigned early_tables_used;
static uint64_t table_offset;

static uint64_t *allocate_table(void) {
    if (early_tables_used >= EARLY_TABLES) return (uint64_t *)0;
    uint64_t *table = early_tables[early_tables_used++];
    for (unsigned index = 0; index < TABLE_ENTRIES; index++) table[index] = 0;
    return table;
}

static uint64_t *descend(uint64_t *table, unsigned index) {
    if (!(table[index] & DESC_VALID)) {
        uint64_t *next = allocate_table();
        if (!next) return (uint64_t *)0;
        table[index] = ((uint64_t)next + table_offset) | DESC_TABLE;
        return next;
    }
    return (uint64_t *)((table[index] & ADDRESS_MASK) - table_offset);
}

static int is_block(uint64_t entry) {
    return (entry & DESC_TABLE) == DESC_VALID;
}

static int map_into(uint64_t *root, uint64_t virtual_address, uint64_t physical,
                    uint64_t attributes, int level) {
    uint64_t *l1 = descend(root, (unsigned)(virtual_address >> 39) & 0x1FFU);
    if (!l1) return -1;
    unsigned i1 = (unsigned)(virtual_address >> 30) & 0x1FFU;
    if (level == 1) {
        l1[i1] = physical | attributes | DESC_VALID;
        return 0;
    }
    if (is_block(l1[i1])) return 0;
    uint64_t *l2 = descend(l1, i1);
    if (!l2) return -1;
    unsigned i2 = (unsigned)(virtual_address >> 21) & 0x1FFU;
    if (level == 2) {
        if (!(l2[i2] & DESC_VALID)) l2[i2] = physical | attributes | DESC_VALID;
        return 0;
    }
    if (is_block(l2[i2])) return 0;
    uint64_t *l3 = descend(l2, i2);
    if (!l3) return -1;
    unsigned i3 = (unsigned)(virtual_address >> 12) & 0x1FFU;
    if (!(l3[i3] & DESC_VALID)) l3[i3] = physical | attributes | DESC_PAGE;
    return 0;
}

void aarch64_build_early_tables(uint64_t load_physical, uint64_t dtb_physical) {
    table_offset = 0;
    uint64_t image_bytes = (uint64_t)(__image_end - kernel_image_start);
    uint64_t kernel_attributes = DESC_AF | DESC_SH_INNER | DESC_UXN;
    uint64_t data_attributes = kernel_attributes | DESC_PXN;

    if (load_physical & (BLOCK_2M - 1U)) {
        for (uint64_t offset = 0; offset < image_bytes; offset += 4096U) {
            map_into(boot_root, load_physical + offset, load_physical + offset,
                     kernel_attributes, 3);
            map_into(boot_root, AARCH64_KERNEL_VIRTUAL_BASE + offset, load_physical + offset,
                     kernel_attributes, 3);
        }
    } else {
        for (uint64_t offset = 0; offset < image_bytes; offset += BLOCK_2M) {
            map_into(boot_root, load_physical + offset, load_physical + offset,
                     kernel_attributes, 2);
            map_into(boot_root, AARCH64_KERNEL_VIRTUAL_BASE + offset, load_physical + offset,
                     kernel_attributes, 2);
        }
    }
    if (dtb_physical) {
        const uint8_t *header = (const uint8_t *)dtb_physical;
        uint64_t bytes = 4096U;
        if (header[0] == 0xD0 && header[1] == 0x0D && header[2] == 0xFE && header[3] == 0xED)
            bytes = ((uint64_t)header[4] << 24) | ((uint64_t)header[5] << 16) |
                    ((uint64_t)header[6] << 8) | header[7];
        uint64_t first = dtb_physical & ~0xFFFULL;
        uint64_t last = dtb_physical + bytes;
        for (uint64_t page = first; page < last; page += 4096U)
            map_into(boot_root, page, page, data_attributes, 3);
    }
}

int aarch64_early_map(uint64_t virtual_address, uint64_t physical, uint64_t attributes,
                      int level) {
    table_offset = aarch64_platform.load_offset;
    int status = map_into(boot_root, virtual_address, physical, attributes, level);
    __asm__ volatile("tlbi vmalle1; dsb sy; isb" ::: "memory");
    return status;
}

uint64_t aarch64_early_map_device(uint64_t physical, uint64_t bytes) {
    static unsigned used;
    uint64_t first = physical & ~0xFFFULL;
    uint64_t pages = ((physical & 0xFFFULL) + bytes + 0xFFFULL) >> 12;
    if (!bytes || used + pages > AARCH64_EARLY_DEVICE_PAGES) return 0;

    uint64_t base = AARCH64_EARLY_DEVICE_BASE + (uint64_t)used * 4096ULL;
    uint64_t attributes = DESC_AF | DESC_ATTR_DEVICE | DESC_PXN | DESC_UXN;
    for (uint64_t page = 0; page < pages; page++) {
        if (aarch64_early_map(base + page * 4096ULL, first + page * 4096ULL,
                              attributes, 3) != 0) return 0;
    }
    used += (unsigned)pages;
    return base + (physical & 0xFFFULL);
}
