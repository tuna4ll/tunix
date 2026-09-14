#include <stdint.h>

#include "include/percpu.h"
#include "include/syscall.h"

#define IA32_GS_BASE 0xC0000101U
#define IA32_KERNEL_GS_BASE 0xC0000102U
#define IDLE_STACK_BYTES 16384

static struct cpu cpus[SMP_MAX_CPUS];
static uint8_t idle_stacks[SMP_MAX_CPUS][IDLE_STACK_BYTES] __attribute__((aligned(16)));

#if defined(__x86_64__)
static inline void write_msr(uint32_t msr, uint64_t value) {
    __asm__ volatile("wrmsr" : : "c"(msr), "a"((uint32_t)value),
                     "d"((uint32_t)(value >> 32)));
}
#endif

struct cpu *percpu_slot(unsigned index) {
    return index < SMP_MAX_CPUS ? &cpus[index] : (struct cpu *)0;
}

void percpu_activate(unsigned index) {
    if (index >= SMP_MAX_CPUS) return;
    struct cpu *cpu = &cpus[index];
    cpu->self = cpu;
    cpu->index = index;
    cpu->idle_stack_top = (uint64_t)(idle_stacks[index] + IDLE_STACK_BYTES);
#if defined(__x86_64__)
    write_msr(IA32_GS_BASE, (uint64_t)cpu);
    write_msr(IA32_KERNEL_GS_BASE, 0);
#elif defined(__aarch64__)
    __asm__ volatile("msr tpidr_el1, %0" : : "r"((uint64_t)cpu));
#endif
}

uint64_t percpu_boot_stack(unsigned index) {
    if (index >= SMP_MAX_CPUS) return 0;
    return (uint64_t)(idle_stacks[index] + IDLE_STACK_BYTES);
}

void percpu_mark_online(unsigned index) {
    if (index < SMP_MAX_CPUS) cpus[index].online = 1;
}

unsigned percpu_online_count(void) {
    unsigned count = 0;
    for (unsigned index = 0; index < SMP_MAX_CPUS; index++)
        if (cpus[index].online) count++;
    return count;
}

void syscall_set_kernel_stack(uint64_t stack_top) {
    cpu_current()->kernel_rsp = stack_top;
}
