#ifndef TUNIX_SYSCALL_H
#define TUNIX_SYSCALL_H

#include <stdint.h>

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
    uint64_t user_rip;
    uint64_t user_rflags;
    uint64_t user_rsp;
};

void syscall_init(void);
void syscall_set_kernel_stack(uint64_t stack_top);
void syscall_dispatch(struct syscall_frame *frame);

#endif
