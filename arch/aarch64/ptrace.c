#include <stddef.h>
#include <stdint.h>
#include <tunix/errno.h>
#include <tunix/kstring.h>
#include <tunix/process.h>
#include <tunix/process_arch.h>
#include <tunix/ptrace.h>
#include <tunix/vmm.h>

#define USER_REGS_BYTES  272
#define FPSIMD_BYTES     528
#define FPSIMD_VREGS     512
#define FPU_FPSR_OFFSET  512
#define FPU_FPCR_OFFSET  520

int arch_ptrace_get_regs(const struct process *tracee, const struct syscall_frame *frame,
                         const uint8_t *fpu, unsigned type, uint8_t *out, size_t *size) {
    if (type == NT_PRSTATUS) {
        memcpy(out, frame->x, sizeof(frame->x));
        memcpy(out + 248, &frame->sp_el0, 8);
        memcpy(out + 256, &frame->elr, 8);
        memcpy(out + 264, &frame->spsr, 8);
        *size = USER_REGS_BYTES;
        return 0;
    }
    if (type == NT_PRFPREG) {
        uint64_t fpsr, fpcr;
        memcpy(&fpsr, fpu + FPU_FPSR_OFFSET, 8);
        memcpy(&fpcr, fpu + FPU_FPCR_OFFSET, 8);
        uint32_t tail[4] = {(uint32_t)fpsr, (uint32_t)fpcr, 0, 0};
        memcpy(out, fpu, FPSIMD_VREGS);
        memcpy(out + FPSIMD_VREGS, tail, sizeof(tail));
        *size = FPSIMD_BYTES;
        return 0;
    }
    if (type == NT_ARM_TLS) {
        memcpy(out, &tracee->fs_base, 8);
        *size = 8;
        return 0;
    }
    return -EINVAL;
}

int arch_ptrace_set_regs(struct process *tracee, struct syscall_frame *frame, uint8_t *fpu,
                         unsigned type, const uint8_t *in) {
    if (type == NT_PRSTATUS) {
        uint64_t sp, pc, pstate;
        memcpy(&sp, in + 248, 8);
        memcpy(&pc, in + 256, 8);
        memcpy(&pstate, in + 264, 8);
        if (sp >= USER_ADDRESS_LIMIT || pc >= USER_ADDRESS_LIMIT) return -EIO;
        memcpy(frame->x, in, sizeof(frame->x));
        frame->sp_el0 = sp;
        frame->elr = pc;
        frame->spsr = pstate & ARCH_SPSR_USER_FLAGS;
        return 0;
    }
    if (type == NT_PRFPREG) {
        uint32_t tail[2];
        memcpy(tail, in + FPSIMD_VREGS, sizeof(tail));
        uint64_t fpsr = tail[0], fpcr = tail[1];
        memcpy(fpu, in, FPSIMD_VREGS);
        memcpy(fpu + FPU_FPSR_OFFSET, &fpsr, 8);
        memcpy(fpu + FPU_FPCR_OFFSET, &fpcr, 8);
        return 0;
    }
    if (type == NT_ARM_TLS) {
        memcpy(&tracee->fs_base, in, 8);
        return 0;
    }
    return -EINVAL;
}
