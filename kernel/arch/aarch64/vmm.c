#include <stddef.h>
#include <stdint.h>

#include "arch.h"

#define ADDR_MASK 0x0000FFFFFFFFF000UL

#define DESC_VALID (1UL << 0)
#define DESC_TABLE (3UL << 0)
#define DESC_PAGE  (3UL << 0)
#define DESC_AF    (1UL << 10)
#define DESC_SH_IN (3UL << 8)
#define DESC_PXN   (1UL << 53)
#define DESC_UXN   (1UL << 54)
#define ATTR_IDX(n) ((uint64_t)(n) << 2)

static uint64_t *table_at(uint64_t pa) {
    return (uint64_t *)phys_to_virt(pa);
}

static uint64_t kernel_root(void) {
    return virt_to_phys((uint64_t)mmu_root_table());
}

static uint64_t *next_table(uint64_t *table, unsigned index) {
    if (!(table[index] & DESC_VALID)) {
        void *fresh = pmm_alloc_page();
        if (!fresh) return NULL;
        table[index] = ((uint64_t)fresh & ADDR_MASK) | DESC_TABLE;
    }
    return table_at(table[index] & ADDR_MASK);
}

static void invalidate(uint64_t va) {
    __asm__ volatile("dsb ishst; tlbi vaae1is, %0; dsb ish; isb"
                     : : "r"(va >> 12) : "memory");
}

int vmm_map(uint64_t root_pa, uint64_t va, uint64_t pa, unsigned flags) {
    uint64_t *l0 = table_at(root_pa);
    uint64_t *l1 = next_table(l0, (va >> 39) & 0x1FF);
    if (!l1) return -1;
    uint64_t *l2 = next_table(l1, (va >> 30) & 0x1FF);
    if (!l2) return -1;
    uint64_t *l3 = next_table(l2, (va >> 21) & 0x1FF);
    if (!l3) return -1;

    uint64_t desc = (pa & ADDR_MASK) | DESC_PAGE | DESC_AF | DESC_SH_IN |
                    ATTR_IDX(1);

    if (flags & VMM_USER) desc |= (flags & VMM_WRITE) ? (1UL << 6) : (3UL << 6);
    else if (!(flags & VMM_WRITE)) desc |= (2UL << 6);

    // Only one exception level may ever execute a given page.
    if (!(flags & VMM_EXEC)) desc |= DESC_PXN | DESC_UXN;
    else if (flags & VMM_USER) desc |= DESC_PXN;
    else desc |= DESC_UXN;

    l3[(va >> 12) & 0x1FF] = desc;
    invalidate(va);
    return 0;
}

int vmm_unmap(uint64_t root_pa, uint64_t va) {
    uint64_t *table = table_at(root_pa);
    unsigned shift[3] = {39, 30, 21};
    for (int level = 0; level < 3; level++) {
        unsigned index = (va >> shift[level]) & 0x1FF;
        if (!(table[index] & DESC_VALID)) return -1;
        table = table_at(table[index] & ADDR_MASK);
    }
    unsigned index = (va >> 12) & 0x1FF;
    if (!(table[index] & DESC_VALID)) return -1;
    table[index] = 0;
    invalidate(va);
    return 0;
}

int vmm_map_page(uint64_t va, uint64_t pa, int writable) {
    return vmm_map(kernel_root(), va, pa, writable ? VMM_WRITE : 0);
}

int vmm_unmap_page(uint64_t va) {
    return vmm_unmap(kernel_root(), va);
}

uint64_t vmm_create_space(void) {
    return (uint64_t)pmm_alloc_page();          // pmm hands back a zeroed frame
}

void vmm_switch_space(uint64_t root_pa) {
    sysreg_write("ttbr0_el1", root_pa);
    isb();
    __asm__ volatile("tlbi vmalle1is; dsb ish; isb" ::: "memory");
}

static void free_level(uint64_t table_pa, int level) {
    uint64_t *table = table_at(table_pa);
    if (level < 3)
        for (int i = 0; i < 512; i++)
            if ((table[i] & DESC_VALID) && (table[i] & 3) == DESC_TABLE)
                free_level(table[i] & ADDR_MASK, level + 1);
    pmm_free_page((void *)table_pa);
}

void vmm_destroy_space(uint64_t root_pa) {
    // Frees the page tables only; mapped frames belong to the caller.
    if (root_pa) free_level(root_pa, 0);
}
