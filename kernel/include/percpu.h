#ifndef TUNIX_PERCPU_H
#define TUNIX_PERCPU_H

#include <stdint.h>

#define SMP_MAX_CPUS 8

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
void percpu_activate(unsigned index);
unsigned percpu_online_count(void);
void percpu_mark_online(unsigned index);
uint64_t percpu_boot_stack(unsigned index);

#endif
