#include <stdint.h>

#include <tunix/kstring.h>
#include <tunix/process.h>
#include <tunix/process_arch.h>

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
        __asm__ volatile("xsave64 (%0)"
                         :
                         : "r"(area), "a"((uint32_t)fpu_xstate_mask),
                           "d"((uint32_t)(fpu_xstate_mask >> 32))
                         : "memory");
    else __asm__ volatile("fxsave64 (%0)" : : "r"(area) : "memory");
}

void arch_fpu_restore(uint8_t *area) {
    if (fpu_xstate_mask)
        __asm__ volatile("xrstor64 (%0)"
                         :
                         : "r"(area), "a"((uint32_t)fpu_xstate_mask),
                           "d"((uint32_t)(fpu_xstate_mask >> 32))
                         : "memory");
    else __asm__ volatile("fxrstor64 (%0)" : : "r"(area) : "memory");
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

// ---- The FPU state in a signal frame, as Linux lays it out ----
//
// The handler runs on the same registers as the code it interrupted, so their vector
// state has to be saved with the rest of the context and put back by sigreturn. Linux
// stores an FXSAVE/XSAVE image and marks the XSAVE extension in the FXSAVE area's
// software-reserved words; userspace (and debuggers) read it through uc_mcontext.fpregs.

#define FP_XSTATE_MAGIC1  0x46505853U
#define FP_XSTATE_MAGIC2  0x46505845U
#define FX_SW_RESERVED    464U
#define FX_MXCSR          24U
#define FX_MXCSR_MASK     28U
#define XSAVE_HEADER      512U
#define FXSAVE_BYTES      512U

uint32_t arch_fpu_signal_size(void) {
    return fpu_xstate_mask ? fpu_xstate_size + 4U : FXSAVE_BYTES;
}

void arch_fpu_signal_save(uint8_t *area) {
    uint32_t size = arch_fpu_signal_size();
    memset(area, 0, size);
    arch_fpu_save(area);
    if (!fpu_xstate_mask) return;
    uint32_t words[3] = {FP_XSTATE_MAGIC1, fpu_xstate_size + 4U, 0};
    memcpy(area + FX_SW_RESERVED, words, 8);
    memcpy(area + FX_SW_RESERVED + 8, &fpu_xstate_mask, 8);
    memcpy(area + FX_SW_RESERVED + 16, &fpu_xstate_size, 4);
    uint32_t magic2 = FP_XSTATE_MAGIC2;
    memcpy(area + fpu_xstate_size, &magic2, 4);
}

// Loads a state userspace handed back. What would fault XRSTOR -- reserved MXCSR bits,
// features this CPU does not have, a compacted header -- is cleared rather than trusted.
void arch_fpu_signal_load(uint8_t *area) {
    uint32_t mxcsr, mxcsr_mask;
    memcpy(&mxcsr, area + FX_MXCSR, 4);
    memcpy(&mxcsr_mask, area + FX_MXCSR_MASK, 4);
    if (!mxcsr_mask) mxcsr_mask = 0xFFBFU;
    mxcsr &= mxcsr_mask & 0xFFFFU;
    memcpy(area + FX_MXCSR, &mxcsr, 4);
    if (fpu_xstate_mask) {
        uint32_t magic1, magic2 = 0;
        memcpy(&magic1, area + FX_SW_RESERVED, 4);
        memcpy(&magic2, area + fpu_xstate_size, 4);
        uint64_t features = 0x3ULL;  // x87 and SSE are always in the FXSAVE part
        if (magic1 == FP_XSTATE_MAGIC1 && magic2 == FP_XSTATE_MAGIC2)
            memcpy(&features, area + XSAVE_HEADER, 8);
        features &= fpu_xstate_mask;
        memset(area + XSAVE_HEADER, 0, 64);
        memcpy(area + XSAVE_HEADER, &features, 8);
    }
    arch_fpu_restore(area);
}
