#ifndef TUNIX_PERCPU_H
#define TUNIX_PERCPU_H

#include <stdint.h>

/*
 * What a processor knows about itself.
 *
 * Every other name in the kernel is shared by all processors, so this block is
 * reached through GS rather than by address: `mov gs:16, rax` gives whichever
 * processor executes it its own block and nobody else's. GS holds the block
 * only while in kernel mode -- the entry and exit stubs swapgs, and the user
 * side of the pair is whatever the process had.
 *
 * kernel_rsp and user_rsp are read by the entry and exit stubs at these exact
 * offsets, before there is a stack to call anything with, so they must stay
 * first; the assertions below are what say so.
 */
#define SMP_MAX_CPUS 8

struct process;

struct cpu {
    uint64_t kernel_rsp;
    uint64_t user_rsp;
    struct cpu *self;
    uint32_t index;
    uint32_t apic_id;
    struct process *current;
    /* The address space loaded here, so that a processor changing a mapping
       can tell which of the others are looking at it. Zero while idle. */
    uint64_t address_space;
    /* Set by a processor asking this one to drop its cached translations, and
       cleared by this one once it has. */
    volatile uint32_t flush_pending;
    uint64_t idle_stack_top;
    /* Written by the processor itself once it has finished coming up, read by
       the one that started it, which is why it is volatile. */
    volatile int online;
};

_Static_assert(__builtin_offsetof(struct cpu, kernel_rsp) == 0, "syscall_entry.S reads gs:0");
_Static_assert(__builtin_offsetof(struct cpu, user_rsp) == 8, "syscall_entry.S reads gs:8");
_Static_assert(__builtin_offsetof(struct cpu, self) == 16, "cpu_current reads gs:16");

static inline struct cpu *cpu_current(void) {
    struct cpu *self;
    __asm__ volatile("movq %%gs:16, %0" : "=r"(self));
    return self;
}

/* The block for a processor that is not running this code -- the bring-up path
   filling in a processor's details before it exists. */
struct cpu *percpu_slot(unsigned index);
/* Point this processor's GS at its block. Must run after the GDT is loaded:
   loading a segment register clears the base the MSR just set. */
void percpu_activate(unsigned index);
unsigned percpu_online_count(void);
void percpu_mark_online(unsigned index);
uint64_t percpu_boot_stack(unsigned index);

#endif
