#ifndef TUNIX_INTERRUPT_H
#define TUNIX_INTERRUPT_H

#include <stdint.h>

#if defined(__x86_64__)

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

_Static_assert(sizeof(struct interrupt_frame) == 184, "isr.S assumes 184");
_Static_assert(__builtin_offsetof(struct interrupt_frame, cs) == 152,
               "isr.S reads cs at 152");

#elif defined(__aarch64__)

struct interrupt_frame {
    uint64_t x[31];
    uint64_t elr;
    uint64_t spsr;
    uint64_t sp_el0;
    uint64_t reserved[2];
};

_Static_assert(sizeof(struct interrupt_frame) == 288, "exceptions.S subtracts 288");

#endif

#endif
