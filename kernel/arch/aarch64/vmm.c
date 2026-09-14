#include <stddef.h>
#include <stdint.h>

#include "arch.h"

#define PAGE_SIZE 4096UL
#define ADDR_MASK 0x0000FFFFFFFFF000UL

#define DESC_VALID (1UL << 0)
#define DESC_TABLE (3UL << 0)
#define DESC_PAGE  (3UL << 0)
#define DESC_AF    (1UL << 10)
#define DESC_SH_IN (3UL << 8)
#define DESC_PXN   (1UL << 53)
#define DESC_UXN   (1UL << 54)
#define ATTR_IDX(n) ((uint64_t)(n) << 2)

uint64_t *mmu_root_table(void);

static uint64_t *next_table(uint64_t *table, unsigned index) {
    if (!(table[index] & DESC_VALID)) {
        uint64_t *fresh = pmm_alloc_page();
        if (!fresh) return NULL;
        table[index] = ((uint64_t)fresh & ADDR_MASK) | DESC_TABLE;
    }
    return (uint64_t *)(table[index] & ADDR_MASK);   // identity-mapped
}

static void invalidate(uint64_t va) {
    __asm__ volatile("dsb ishst; tlbi vaae1is, %0; dsb ish; isb"
                     : : "r"(va >> 12) : "memory");
}

int vmm_map_page(uint64_t va, uint64_t pa, int writable) {
    uint64_t *l0 = mmu_root_table();
    uint64_t *l1 = next_table(l0, (va >> 39) & 0x1FF);
    if (!l1) return -1;
    uint64_t *l2 = next_table(l1, (va >> 30) & 0x1FF);
    if (!l2) return -1;
    uint64_t *l3 = next_table(l2, (va >> 21) & 0x1FF);
    if (!l3) return -1;

    uint64_t desc = (pa & ADDR_MASK) | DESC_PAGE | DESC_AF | DESC_SH_IN |
                    ATTR_IDX(1) | DESC_PXN | DESC_UXN;
    if (!writable) desc |= (2UL << 6);              // AP read-only
    l3[(va >> 12) & 0x1FF] = desc;
    invalidate(va);
    return 0;
}

int vmm_unmap_page(uint64_t va) {
    uint64_t *table = mmu_root_table();
    unsigned shift[4] = {39, 30, 21, 12};
    for (int level = 0; level < 3; level++) {
        unsigned index = (va >> shift[level]) & 0x1FF;
        if (!(table[index] & DESC_VALID)) return -1;
        table = (uint64_t *)(table[index] & ADDR_MASK);
    }
    unsigned index = (va >> 12) & 0x1FF;
    if (!(table[index] & DESC_VALID)) return -1;
    table[index] = 0;
    invalidate(va);
    return 0;
}
