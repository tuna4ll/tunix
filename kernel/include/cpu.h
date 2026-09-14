#ifndef TUNIX_CPU_H
#define TUNIX_CPU_H

#include <stdint.h>

#if defined(__x86_64__)

static inline void cpu_relax(void) {
    __asm__ volatile("pause");
}

#elif defined(__aarch64__)

static inline void cpu_relax(void) {
    __asm__ volatile("yield");
}

#else
#error "no processor primitives for this architecture"
#endif

#endif
