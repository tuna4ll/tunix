#include <stdint.h>

#include "arch.h"

#define PT_ENTRIES 512

#define DESC_VALID   (1UL << 0)
#define DESC_TABLE   (3UL << 0)
#define DESC_BLOCK   (1UL << 0)
#define DESC_AF      (1UL << 10)
#define DESC_SH_INNER (3UL << 8)
#define DESC_PXN     (1UL << 53)
#define DESC_UXN     (1UL << 54)
#define ATTR_IDX(n)  ((uint64_t)(n) << 2)

#define MAIR_DEVICE_nGnRnE 0x00UL
#define MAIR_NORMAL_WB     0xFFUL

static uint64_t l0_table[PT_ENTRIES] __attribute__((aligned(4096)));
static uint64_t l1_table[PT_ENTRIES] __attribute__((aligned(4096)));

uint64_t *mmu_root_table(void) {
    return l0_table;
}

void mmu_init(void) {
    for (int i = 0; i < PT_ENTRIES; i++) {
        l0_table[i] = 0;
        l1_table[i] = 0;
    }

    l0_table[0] = (uint64_t)l1_table | DESC_TABLE;

    // 0-1 GiB: MMIO (UART, GIC) as device, never executable.
    l1_table[0] = 0x00000000UL | DESC_BLOCK | DESC_AF | ATTR_IDX(0) |
                  DESC_PXN | DESC_UXN;

    // 1-4 GiB: RAM as normal write-back, inner shareable.
    for (uint64_t gb = 1; gb < 4; gb++)
        l1_table[gb] = (gb << 30) | DESC_BLOCK | DESC_AF | DESC_SH_INNER | ATTR_IDX(1);

    uint64_t mair = MAIR_DEVICE_nGnRnE | (MAIR_NORMAL_WB << 8);
    sysreg_write("mair_el1", mair);

    uint64_t tcr = 16UL |            // T0SZ = 48-bit VA
                   (1UL << 8) |      // IRGN0 write-back
                   (1UL << 10) |     // ORGN0 write-back
                   (3UL << 12) |     // SH0 inner shareable
                   (0UL << 14) |     // TG0 4 KiB
                   (1UL << 23) |     // EPD1: no TTBR1 walks
                   (2UL << 32);      // IPS 40-bit PA
    sysreg_write("tcr_el1", tcr);
    sysreg_write("ttbr0_el1", (uint64_t)l0_table);
    isb();

    __asm__ volatile("tlbi vmalle1; dsb sy; isb" ::: "memory");

    uint64_t sctlr = sysreg_read("sctlr_el1");
    sctlr |= (1UL << 0) | (1UL << 2) | (1UL << 12);   // M, C, I
    sysreg_write("sctlr_el1", sctlr);
    isb();
}
