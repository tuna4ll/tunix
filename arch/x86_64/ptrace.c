#include <stddef.h>
#include <stdint.h>
#include <tunix/errno.h>
#include <tunix/kstring.h>
#include <tunix/process.h>
#include <tunix/ptrace.h>
#include <tunix/vmm.h>

#define USER_REGS_WORDS 27
#define FXSAVE_BYTES    512
#define RFLAGS_USER     0x50DD5ULL

enum {
    REG_R15, REG_R14, REG_R13, REG_R12, REG_RBP, REG_RBX, REG_R11, REG_R10, REG_R9, REG_R8,
    REG_RAX, REG_RCX, REG_RDX, REG_RSI, REG_RDI, REG_ORIG_RAX, REG_RIP, REG_CS, REG_EFLAGS,
    REG_RSP, REG_SS, REG_FS_BASE, REG_GS_BASE, REG_DS, REG_ES, REG_FS, REG_GS
};

int arch_ptrace_get_regs(const struct process *tracee, const struct syscall_frame *frame,
                         const uint8_t *fpu, unsigned type, uint8_t *out, size_t *size) {
    if (type == NT_PRFPREG) {
        memcpy(out, fpu, FXSAVE_BYTES);
        *size = FXSAVE_BYTES;
        return 0;
    }
    if (type != NT_PRSTATUS) return -EINVAL;
    uint64_t regs[USER_REGS_WORDS] = {0};
    regs[REG_R15] = frame->r15;
    regs[REG_R14] = frame->r14;
    regs[REG_R13] = frame->r13;
    regs[REG_R12] = frame->r12;
    regs[REG_RBP] = frame->rbp;
    regs[REG_RBX] = frame->rbx;
    regs[REG_R11] = frame->r11;
    regs[REG_R10] = frame->r10;
    regs[REG_R9] = frame->r9;
    regs[REG_R8] = frame->r8;
    regs[REG_RAX] = frame->rax;
    regs[REG_RCX] = frame->rcx;
    regs[REG_RDX] = frame->rdx;
    regs[REG_RSI] = frame->rsi;
    regs[REG_RDI] = frame->rdi;
    regs[REG_ORIG_RAX] = (uint64_t)tracee->ptrace_syscall_nr;
    regs[REG_RIP] = frame->user_rip;
    regs[REG_CS] = 0x23;
    regs[REG_EFLAGS] = frame->user_rflags;
    regs[REG_RSP] = frame->user_rsp;
    regs[REG_SS] = 0x1b;
    regs[REG_FS_BASE] = tracee->fs_base;
    regs[REG_GS_BASE] = tracee->gs_base;
    memcpy(out, regs, sizeof(regs));
    *size = sizeof(regs);
    return 0;
}

int arch_ptrace_set_regs(struct process *tracee, struct syscall_frame *frame, uint8_t *fpu,
                         unsigned type, const uint8_t *in) {
    if (type == NT_PRFPREG) {
        uint32_t mask;
        memcpy(&mask, fpu + 28, sizeof(mask));
        if (!mask) mask = 0xFFBF;
        uint32_t mxcsr;
        memcpy(&mxcsr, in + 24, sizeof(mxcsr));
        if (mxcsr & ~mask) return -EINVAL;
        memcpy(fpu, in, 24);
        memcpy(fpu + 24, &mxcsr, sizeof(mxcsr));
        memcpy(fpu + 32, in + 32, FXSAVE_BYTES - 32);
        fpu[FXSAVE_BYTES] |= 3U;
        return 0;
    }
    if (type != NT_PRSTATUS) return -EINVAL;
    uint64_t regs[USER_REGS_WORDS];
    memcpy(regs, in, sizeof(regs));
    if (regs[REG_RIP] >= USER_ADDRESS_LIMIT || regs[REG_RSP] >= USER_ADDRESS_LIMIT ||
        regs[REG_FS_BASE] >= USER_ADDRESS_LIMIT || regs[REG_GS_BASE] >= USER_ADDRESS_LIMIT)
        return -EIO;
    frame->r15 = regs[REG_R15];
    frame->r14 = regs[REG_R14];
    frame->r13 = regs[REG_R13];
    frame->r12 = regs[REG_R12];
    frame->rbp = regs[REG_RBP];
    frame->rbx = regs[REG_RBX];
    frame->r11 = regs[REG_R11];
    frame->r10 = regs[REG_R10];
    frame->r9 = regs[REG_R9];
    frame->r8 = regs[REG_R8];
    frame->rax = regs[REG_RAX];
    frame->rcx = regs[REG_RCX];
    frame->rdx = regs[REG_RDX];
    frame->rsi = regs[REG_RSI];
    frame->rdi = regs[REG_RDI];
    frame->user_rip = regs[REG_RIP];
    frame->user_rflags = (regs[REG_EFLAGS] & RFLAGS_USER) | 0x202ULL;
    frame->user_rsp = regs[REG_RSP];
    tracee->fs_base = regs[REG_FS_BASE];
    tracee->gs_base = regs[REG_GS_BASE];
    tracee->ptrace_syscall_nr = (int64_t)regs[REG_ORIG_RAX];
    return 0;
}
