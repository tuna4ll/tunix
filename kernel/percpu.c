#include <stdint.h>

#include "include/percpu.h"
#include "include/syscall.h"

#define IA32_GS_BASE 0xC0000101U
#define IA32_KERNEL_GS_BASE 0xC0000102U
#define IDLE_STACK_BYTES 16384

static struct cpu cpus[SMP_MAX_CPUS];
/*
 * The stack a processor runs on when it has no process to run. It is also the
 * stack an application processor starts on, before it has one -- which is why
 * it is here and not allocated: it has to exist before the heap does.
 */
static uint8_t idle_stacks[SMP_MAX_CPUS][IDLE_STACK_BYTES] __attribute__((aligned(16)));

static inline void write_msr(uint32_t msr, uint64_t value) {
    __asm__ volatile("wrmsr" : : "c"(msr), "a"((uint32_t)value),
                     "d"((uint32_t)(value >> 32)));
}

struct cpu *percpu_slot(unsigned index) {
    return index < SMP_MAX_CPUS ? &cpus[index] : (struct cpu *)0;
}

void percpu_activate(unsigned index) {
    if (index >= SMP_MAX_CPUS) return;
    struct cpu *cpu = &cpus[index];
    cpu->self = cpu;
    cpu->index = index;
    cpu->idle_stack_top = (uint64_t)(idle_stacks[index] + IDLE_STACK_BYTES);
    /* GS holds the block now; the kernel side of the swapgs pair is cleared so
       that the first return to user mode swaps the block out of sight and the
       next entry swaps it back. */
    write_msr(IA32_GS_BASE, (uint64_t)cpu);
    write_msr(IA32_KERNEL_GS_BASE, 0);
}

/* The stack a processor starts on, wanted before it exists and so before it
   can be asked for its own. */
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
