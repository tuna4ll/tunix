#ifndef TUNIX_PERCPU_H
#define TUNIX_PERCPU_H

#include <stdint.h>

#define SMP_MAX_CPUS 256

struct cpu_mask {
    uint64_t bits[SMP_MAX_CPUS / 64];
};

static inline int cpu_mask_test(const struct cpu_mask *mask, unsigned index) {
    return index < SMP_MAX_CPUS && ((mask->bits[index / 64] >> (index % 64)) & 1U);
}

static inline void cpu_mask_set(struct cpu_mask *mask, unsigned index) {
    if (index < SMP_MAX_CPUS) mask->bits[index / 64] |= 1ULL << (index % 64);
}

static inline int cpu_mask_empty(const struct cpu_mask *mask) {
    for (unsigned word = 0; word < SMP_MAX_CPUS / 64; word++)
        if (mask->bits[word]) return 0;
    return 1;
}

struct process;

struct cpu {
    uint64_t kernel_rsp;
    uint64_t user_rsp;
    struct cpu *self;
    uint32_t index;
    uint32_t apic_id;
    struct process *current;
    uint64_t address_space;
    volatile uint32_t flush_pending;
    uint64_t idle_stack_top;
    volatile int online;
    struct process *switch_owner;
    uint8_t switching;
};

_Static_assert(__builtin_offsetof(struct cpu, kernel_rsp) == 0, "syscall_entry.S reads gs:0");
_Static_assert(__builtin_offsetof(struct cpu, user_rsp) == 8, "syscall_entry.S reads gs:8");
_Static_assert(__builtin_offsetof(struct cpu, self) == 16, "cpu_current reads gs:16");

#if defined(__x86_64__)
static inline struct cpu *cpu_current(void) {
    struct cpu *self;
    __asm__ volatile("movq %%gs:16, %0" : "=r"(self));
    return self;
}
#elif defined(__aarch64__)
static inline struct cpu *cpu_current(void) {
    struct cpu *self;
    __asm__ volatile("mrs %0, tpidr_el1" : "=r"(self));
    return self;
}
#endif

struct cpu *percpu_slot(unsigned index);

static inline struct process *cpu_running(const struct cpu *cpu) {
    return cpu->current;
}
void percpu_activate(unsigned index);
unsigned percpu_online_count(void);
void percpu_mark_online(unsigned index);
uint64_t percpu_boot_stack(unsigned index);

#endif
