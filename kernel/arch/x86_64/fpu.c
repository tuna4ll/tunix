#include <stdint.h>

#include "../../include/kstring.h"
#include "../../include/process.h"
#include "../../include/process_arch.h"

static uint64_t fpu_xstate_mask;
static uint32_t fpu_xstate_size;

void process_enable_extended_fpu(void) {
    uint32_t a, b, c, d;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1), "c"(0));
    if (!(c & (1U << 26))) return;

    uint64_t cr4;
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= 1ULL << 18;
    __asm__ volatile("mov %0, %%cr4" : : "r"(cr4));

    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(13U), "c"(0));
    uint64_t wanted = ((uint64_t)a) & 0x7ULL;
    if (!(wanted & 0x3ULL)) return;
    wanted |= 0x3ULL;

    __asm__ volatile("xsetbv" : : "a"((uint32_t)wanted), "d"(0U), "c"(0U));

    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(13U), "c"(0));
    if (b > PROCESS_FPU_STATE_SIZE) {
        cr4 &= ~(1ULL << 18);
        __asm__ volatile("mov %0, %%cr4" : : "r"(cr4));
        return;
    }
    fpu_xstate_size = b;
    fpu_xstate_mask = wanted;
}

void arch_fpu_save(uint8_t *area) {
    if (fpu_xstate_mask)
        __asm__ volatile("xsave64 (%0)" : : "r"(area),
                         "a"((uint32_t)fpu_xstate_mask),
                         "d"((uint32_t)(fpu_xstate_mask >> 32)) : "memory");
    else
        __asm__ volatile("fxsave64 (%0)" : : "r"(area) : "memory");
}

void arch_fpu_restore(uint8_t *area) {
    if (fpu_xstate_mask)
        __asm__ volatile("xrstor64 (%0)" : : "r"(area),
                         "a"((uint32_t)fpu_xstate_mask),
                         "d"((uint32_t)(fpu_xstate_mask >> 32)) : "memory");
    else
        __asm__ volatile("fxrstor64 (%0)" : : "r"(area) : "memory");
}

void arch_fpu_init(uint8_t *area) {
    memset(area, 0, PROCESS_FPU_STATE_SIZE);
    area[0] = 0x7F;
    area[1] = 0x03;
    area[24] = 0x80;
    area[25] = 0x1F;
    area[28] = 0xFF;
    area[29] = 0xFF;
}
