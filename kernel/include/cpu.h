#ifndef TUNIX_CPU_H
#define TUNIX_CPU_H

#include <stdint.h>

#if defined(__x86_64__)

static inline void cpu_relax(void) {
    __asm__ volatile("pause");
}

static inline uint64_t cpu_irq_save(void) {
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(flags) :: "memory");
    return flags;
}

static inline void cpu_irq_restore(uint64_t flags) {
    if (flags & 0x200ULL) __asm__ volatile("sti");
}

static inline void cpu_irq_disable(void) {
    __asm__ volatile("cli");
}

__attribute__((noreturn)) static inline void cpu_halt_forever(void) {
    for (;;) __asm__ volatile("cli; hlt");
}

static inline void cpu_memory_barrier(void) {
    __asm__ volatile("mfence" : : : "memory");
}

static inline uint64_t cpu_counter(void) {
    uint32_t low, high;
    __asm__ volatile("rdtsc" : "=a"(low), "=d"(high));
    return ((uint64_t)high << 32) | low;
}

static inline uint64_t cpu_counter_ordered(void) {
    uint32_t low;
    uint32_t high;
    __asm__ volatile("lfence; rdtsc" : "=a"(low), "=d"(high) : : "memory");
    return ((uint64_t)high << 32) | low;
}

static inline void cpu_cpuid(uint32_t leaf, uint32_t subleaf,
                             uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d) {
    __asm__ volatile("cpuid"
                     : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
                     : "a"(leaf), "c"(subleaf));
}

#elif defined(__aarch64__)

static inline void cpu_relax(void) {
    __asm__ volatile("yield");
}

static inline uint64_t cpu_irq_save(void) {
    uint64_t flags;
    __asm__ volatile("mrs %0, daif; msr daifset, #2" : "=r"(flags) :: "memory");
    return flags;
}

static inline void cpu_irq_restore(uint64_t flags) {
    if (!(flags & 0x80ULL)) __asm__ volatile("msr daifclr, #2");
}

static inline void cpu_irq_disable(void) {
    __asm__ volatile("msr daifset, #2");
}

__attribute__((noreturn)) static inline void cpu_halt_forever(void) {
    for (;;) __asm__ volatile("msr daifset, #2; wfi");
}

static inline void cpu_memory_barrier(void) {
    __asm__ volatile("dsb sy" : : : "memory");
}

static inline uint64_t cpu_counter(void) {
    uint64_t value;
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(value));
    return value;
}

static inline uint64_t cpu_counter_ordered(void) {
    uint64_t value;
    __asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(value) : : "memory");
    return value;
}

#else
#error "no processor primitives for this architecture"
#endif

struct cpu_identity {
    char vendor[13];
    char model[49];
    uint32_t family;
    uint32_t model_number;
    uint32_t stepping;
};

void cpu_identify(struct cpu_identity *out);

#endif
