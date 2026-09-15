#ifndef TUNIX_SYSCALL_H
#define TUNIX_SYSCALL_H

#include <stdint.h>

#include "syscall_abi.h"

#if defined(__x86_64__)

struct syscall_frame {
    uint64_t r15;
    uint64_t r14;
    uint64_t r13;
    uint64_t r12;
    uint64_t rbp;
    uint64_t rbx;
    uint64_t r9;
    uint64_t r8;
    uint64_t r10;
    uint64_t rdx;
    uint64_t rsi;
    uint64_t rdi;
    uint64_t rax;
    uint64_t rcx;
    uint64_t r11;
    uint64_t user_rip;
    uint64_t user_rflags;
    uint64_t user_rsp;
};

_Static_assert(sizeof(struct syscall_frame) == 144, "syscall_entry.S subtracts 144");
_Static_assert(__builtin_offsetof(struct syscall_frame, r15) == 0, "entry writes r15 at 0");
_Static_assert(__builtin_offsetof(struct syscall_frame, rax) == 96, "entry writes rax at 96");
_Static_assert(__builtin_offsetof(struct syscall_frame, rcx) == 104, "entry writes rcx at 104");
_Static_assert(__builtin_offsetof(struct syscall_frame, r11) == 112, "entry writes r11 at 112");
_Static_assert(__builtin_offsetof(struct syscall_frame, user_rip) == 120, "entry writes rip at 120");
_Static_assert(__builtin_offsetof(struct syscall_frame, user_rflags) == 128, "entry writes rflags at 128");
_Static_assert(__builtin_offsetof(struct syscall_frame, user_rsp) == 136, "entry writes rsp at 136");

#elif defined(__aarch64__)

struct syscall_frame {
    uint64_t x[31];
    uint64_t elr;
    uint64_t spsr;
    uint64_t sp_el0;
    uint64_t reserved[2];
};

_Static_assert(sizeof(struct syscall_frame) == 288, "exceptions.S subtracts 288");
_Static_assert(__builtin_offsetof(struct syscall_frame, elr) == 248, "entry writes elr at 248");
_Static_assert(__builtin_offsetof(struct syscall_frame, spsr) == 256, "entry writes spsr at 256");
_Static_assert(__builtin_offsetof(struct syscall_frame, sp_el0) == 264, "entry writes sp_el0 at 264");

#endif

void syscall_init(void);
void syscall_set_kernel_stack(uint64_t stack_top);
void syscall_dispatch(struct syscall_frame *frame);

#endif
