#ifndef TUNIX_INTERRUPT_H
#define TUNIX_INTERRUPT_H

#include <stdint.h>

/*
 * Stack layout produced by src/kernel/arch/x86_64/isr.S.
 * For interrupts raised while CPL=3, rsp/ss are supplied by the CPU and are
 * therefore safe to access. Kernel-mode IRQ handlers must check cs first.
 */
struct interrupt_frame {
    uint64_t ds;
    uint64_t rax;
    uint64_t rbx;
    uint64_t rcx;
    uint64_t rdx;
    uint64_t rsi;
    uint64_t rdi;
    uint64_t rbp;
    uint64_t r8;
    uint64_t r9;
    uint64_t r10;
    uint64_t r11;
    uint64_t r12;
    uint64_t r13;
    uint64_t r14;
    uint64_t r15;
    uint64_t int_no;
    uint64_t err_code;
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t rsp;
    uint64_t ss;
};

#endif
