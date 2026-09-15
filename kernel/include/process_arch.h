#ifndef TUNIX_PROCESS_ARCH_H
#define TUNIX_PROCESS_ARCH_H

#include <stdint.h>

#include "interrupt.h"
#include "kstring.h"
#include "process.h"
#include "signal.h"
#include "syscall.h"
#include "vmm.h"

void arch_fpu_save(uint8_t *area);
void arch_fpu_restore(uint8_t *area);
void arch_fpu_init(uint8_t *area);

#if defined(__x86_64__)

#define IA32_FS_BASE 0xC0000100U

static inline int arch_map_signal_trampoline(uint64_t cr3) {
    (void)cr3;
    return 0;
}

static inline void arch_sanitize_sigaction(struct tunix_sigaction *action) {
    (void)action;
}
#define IA32_KERNEL_GS_BASE 0xC0000102U

static inline void arch_wrmsr(uint32_t msr, uint64_t value) {
    uint32_t low = (uint32_t)value;
    uint32_t high = (uint32_t)(value >> 32);
    __asm__ volatile("wrmsr" : : "c"(msr), "a"(low), "d"(high));
}

static inline void arch_frame_enter_user(struct syscall_frame *frame, uint64_t entry,
                                         uint64_t stack) {
    frame->user_rip = entry;
    frame->user_rsp = stack;
    frame->user_rflags = 0x202;
}

static inline int arch_interrupt_from_user(const struct interrupt_frame *frame) {
    return (frame->cs & 3U) == 3U;
}

static inline void arch_frame_from_interrupt(struct syscall_frame *destination,
                                             const struct interrupt_frame *source) {
    destination->r15 = source->r15;
    destination->r14 = source->r14;
    destination->r13 = source->r13;
    destination->r12 = source->r12;
    destination->rbp = source->rbp;
    destination->rbx = source->rbx;
    destination->r9 = source->r9;
    destination->r8 = source->r8;
    destination->r10 = source->r10;
    destination->rdx = source->rdx;
    destination->rsi = source->rsi;
    destination->rdi = source->rdi;
    destination->rax = source->rax;
    destination->rcx = source->rcx;
    destination->r11 = source->r11;
    destination->user_rip = source->rip;
    destination->user_rflags = source->rflags;
    destination->user_rsp = source->rsp;
}

static inline void arch_frame_to_interrupt(struct interrupt_frame *destination,
                                           const struct syscall_frame *source) {
    destination->ds = 0x1b;
    destination->r15 = source->r15;
    destination->r14 = source->r14;
    destination->r13 = source->r13;
    destination->r12 = source->r12;
    destination->r11 = source->r11;
    destination->r10 = source->r10;
    destination->r9 = source->r9;
    destination->r8 = source->r8;
    destination->rbp = source->rbp;
    destination->rdi = source->rdi;
    destination->rsi = source->rsi;
    destination->rdx = source->rdx;
    destination->rcx = source->rcx;
    destination->rbx = source->rbx;
    destination->rax = source->rax;
    destination->rip = source->user_rip;
    destination->cs = 0x23;
    destination->rflags = source->user_rflags | 0x2ULL;
    destination->rsp = source->user_rsp;
    destination->ss = 0x1b;
}

static inline void arch_load_thread_pointers(uint64_t fs_base, uint64_t gs_base) {
    arch_wrmsr(IA32_FS_BASE, fs_base);
    arch_wrmsr(IA32_KERNEL_GS_BASE, gs_base);
}

static inline void arch_save_thread_pointers(struct process *process) {
    (void)process;
}

static inline void arch_write_fs_base(uint64_t value) {
    arch_wrmsr(IA32_FS_BASE, value);
}

static inline void arch_write_gs_base(uint64_t value) {
    arch_wrmsr(IA32_KERNEL_GS_BASE, value);
}

static inline void arch_mcontext_put(uint8_t *context, unsigned slot, uint64_t value) {
    memcpy(context + UCONTEXT_MCONTEXT_OFFSET + slot * 8U, &value, sizeof(value));
}

static inline uint64_t arch_mcontext_get(const uint8_t *context, unsigned slot) {
    uint64_t value;
    memcpy(&value, context + UCONTEXT_MCONTEXT_OFFSET + slot * 8U, sizeof(value));
    return value;
}

static inline void arch_fill_mcontext(uint8_t *context, const struct syscall_frame *frame) {
    arch_mcontext_put(context, MCONTEXT_R8, frame->r8);
    arch_mcontext_put(context, MCONTEXT_R9, frame->r9);
    arch_mcontext_put(context, MCONTEXT_R10, frame->r10);
    arch_mcontext_put(context, MCONTEXT_R11, frame->r11);
    arch_mcontext_put(context, MCONTEXT_R12, frame->r12);
    arch_mcontext_put(context, MCONTEXT_R13, frame->r13);
    arch_mcontext_put(context, MCONTEXT_R14, frame->r14);
    arch_mcontext_put(context, MCONTEXT_R15, frame->r15);
    arch_mcontext_put(context, MCONTEXT_RDI, frame->rdi);
    arch_mcontext_put(context, MCONTEXT_RSI, frame->rsi);
    arch_mcontext_put(context, MCONTEXT_RBP, frame->rbp);
    arch_mcontext_put(context, MCONTEXT_RBX, frame->rbx);
    arch_mcontext_put(context, MCONTEXT_RDX, frame->rdx);
    arch_mcontext_put(context, MCONTEXT_RAX, frame->rax);
    arch_mcontext_put(context, MCONTEXT_RCX, frame->rcx);
    arch_mcontext_put(context, MCONTEXT_RSP, frame->user_rsp);
    arch_mcontext_put(context, MCONTEXT_RIP, frame->user_rip);
    arch_mcontext_put(context, MCONTEXT_EFLAGS, frame->user_rflags);
}

static inline void arch_read_mcontext(struct syscall_frame *frame, const uint8_t *context) {
    frame->r8 = arch_mcontext_get(context, MCONTEXT_R8);
    frame->r9 = arch_mcontext_get(context, MCONTEXT_R9);
    frame->r10 = arch_mcontext_get(context, MCONTEXT_R10);
    frame->r11 = arch_mcontext_get(context, MCONTEXT_R11);
    frame->r12 = arch_mcontext_get(context, MCONTEXT_R12);
    frame->r13 = arch_mcontext_get(context, MCONTEXT_R13);
    frame->r14 = arch_mcontext_get(context, MCONTEXT_R14);
    frame->r15 = arch_mcontext_get(context, MCONTEXT_R15);
    frame->rdi = arch_mcontext_get(context, MCONTEXT_RDI);
    frame->rsi = arch_mcontext_get(context, MCONTEXT_RSI);
    frame->rbp = arch_mcontext_get(context, MCONTEXT_RBP);
    frame->rbx = arch_mcontext_get(context, MCONTEXT_RBX);
    frame->rdx = arch_mcontext_get(context, MCONTEXT_RDX);
    frame->rax = arch_mcontext_get(context, MCONTEXT_RAX);
    frame->rcx = arch_mcontext_get(context, MCONTEXT_RCX);
    uint64_t rsp = arch_mcontext_get(context, MCONTEXT_RSP);
    uint64_t rip = arch_mcontext_get(context, MCONTEXT_RIP);
    if (rsp && rsp < USER_ADDRESS_LIMIT) frame->user_rsp = rsp;
    if (rip && rip < USER_ADDRESS_LIMIT) frame->user_rip = rip;
    uint64_t flags = arch_mcontext_get(context, MCONTEXT_EFLAGS);
    frame->user_rflags = (flags & ~(uint64_t)0x200D5UL & 0x3F7FD5UL) | 0x202UL;
}

static inline int arch_signal_push_restorer(uint64_t cr3, uint64_t area,
                                            const uint64_t *restorer, uint64_t *stack_out) {
    uint64_t new_rsp = area - 8;
    *stack_out = new_rsp;
    return vmm_copy_to_space(cr3, new_rsp, restorer, sizeof(*restorer));
}

static inline void arch_signal_enter_handler(struct syscall_frame *frame, uint64_t stack,
                                             uint64_t handler, uint64_t restorer,
                                             int signal_number, uint64_t info,
                                             uint64_t context) {
    (void)restorer;
    frame->user_rsp = stack;
    frame->user_rip = handler;
    frame->rdi = (uint64_t)signal_number;
    frame->rsi = info;
    frame->rdx = context;
    frame->rax = 0;
}

#elif defined(__aarch64__)

#define ARCH_SPSR_MODE_MASK 0xFULL
#define ARCH_SIGNAL_TRAMPOLINE 0x00007FFFFFFFF000ULL

int arch_map_signal_trampoline(uint64_t cr3);

static inline void arch_sanitize_sigaction(struct tunix_sigaction *action) {
    if (!(action->flags & SA_RESTORER)) action->restorer = 0;
}
#define ARCH_SPSR_USER_FLAGS 0xF0000000ULL

#define SIGCONTEXT_REGS_OFFSET 8U
#define SIGCONTEXT_SP_OFFSET 256U
#define SIGCONTEXT_PC_OFFSET 264U
#define SIGCONTEXT_PSTATE_OFFSET 272U

static inline void arch_frame_enter_user(struct syscall_frame *frame, uint64_t entry,
                                         uint64_t stack) {
    frame->elr = entry;
    frame->sp_el0 = stack;
    frame->spsr = 0;
}

static inline int arch_interrupt_from_user(const struct interrupt_frame *frame) {
    return (frame->spsr & ARCH_SPSR_MODE_MASK) == 0;
}

static inline void arch_frame_from_interrupt(struct syscall_frame *destination,
                                             const struct interrupt_frame *source) {
    for (unsigned index = 0; index < 31U; index++) destination->x[index] = source->x[index];
    destination->elr = source->elr;
    destination->spsr = source->spsr;
    destination->sp_el0 = source->sp_el0;
}

static inline void arch_frame_to_interrupt(struct interrupt_frame *destination,
                                           const struct syscall_frame *source) {
    for (unsigned index = 0; index < 31U; index++) destination->x[index] = source->x[index];
    destination->elr = source->elr;
    destination->spsr = source->spsr & ARCH_SPSR_USER_FLAGS;
    destination->sp_el0 = source->sp_el0;
}

static inline void arch_load_thread_pointers(uint64_t fs_base, uint64_t gs_base) {
    (void)gs_base;
    __asm__ volatile("msr tpidr_el0, %0" : : "r"(fs_base));
}

static inline void arch_save_thread_pointers(struct process *process) {
    uint64_t value;
    __asm__ volatile("mrs %0, tpidr_el0" : "=r"(value));
    process->fs_base = value;
}

static inline void arch_write_fs_base(uint64_t value) {
    __asm__ volatile("msr tpidr_el0, %0" : : "r"(value));
}

static inline void arch_write_gs_base(uint64_t value) {
    (void)value;
}

static inline void arch_fill_mcontext(uint8_t *context, const struct syscall_frame *frame) {
    uint8_t *sigcontext = context + UCONTEXT_MCONTEXT_OFFSET;
    memcpy(sigcontext + SIGCONTEXT_REGS_OFFSET, frame->x, sizeof(frame->x));
    memcpy(sigcontext + SIGCONTEXT_SP_OFFSET, &frame->sp_el0, sizeof(frame->sp_el0));
    memcpy(sigcontext + SIGCONTEXT_PC_OFFSET, &frame->elr, sizeof(frame->elr));
    memcpy(sigcontext + SIGCONTEXT_PSTATE_OFFSET, &frame->spsr, sizeof(frame->spsr));
}

static inline void arch_read_mcontext(struct syscall_frame *frame, const uint8_t *context) {
    const uint8_t *sigcontext = context + UCONTEXT_MCONTEXT_OFFSET;
    uint64_t sp, pc, pstate;
    memcpy(frame->x, sigcontext + SIGCONTEXT_REGS_OFFSET, sizeof(frame->x));
    memcpy(&sp, sigcontext + SIGCONTEXT_SP_OFFSET, sizeof(sp));
    memcpy(&pc, sigcontext + SIGCONTEXT_PC_OFFSET, sizeof(pc));
    memcpy(&pstate, sigcontext + SIGCONTEXT_PSTATE_OFFSET, sizeof(pstate));
    if (sp && sp < USER_ADDRESS_LIMIT) frame->sp_el0 = sp;
    if (pc && pc < USER_ADDRESS_LIMIT) frame->elr = pc;
    frame->spsr = pstate & ARCH_SPSR_USER_FLAGS;
}

static inline int arch_signal_push_restorer(uint64_t cr3, uint64_t area,
                                            const uint64_t *restorer, uint64_t *stack_out) {
    (void)cr3;
    (void)restorer;
    *stack_out = area & ~15ULL;
    return 0;
}

static inline void arch_signal_enter_handler(struct syscall_frame *frame, uint64_t stack,
                                             uint64_t handler, uint64_t restorer,
                                             int signal_number, uint64_t info,
                                             uint64_t context) {
    frame->sp_el0 = stack;
    frame->elr = handler;
    frame->x[0] = (uint64_t)signal_number;
    frame->x[1] = info;
    frame->x[2] = context;
    frame->x[30] = restorer ? restorer : ARCH_SIGNAL_TRAMPOLINE;
}

#else
#error "no process context helpers for this architecture"
#endif

#endif
