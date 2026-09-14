#ifndef TUNIX_SYSCALL_ABI_H
#define TUNIX_SYSCALL_ABI_H

/*
 * The seam between the portable syscall layer and the register file a given
 * architecture calls with.  These are macros rather than inline functions
 * because the return slot has to be assignable, and because expanding to the
 * struct member directly keeps the generated code identical to what the field
 * access produced before the seam existed.
 *
 * x86-64 carries the number in RAX and the arguments in RDI, RSI, RDX, R10,
 * R8, R9, with the result written back over RAX.  AArch64 carries the number
 * in X8 and the arguments in X0-X5, with the result written back over X0 --
 * which is why the number and the result cannot share one accessor, even
 * though on x86-64 they happen to share a register.
 *
 * The AArch64 mapping lives in kernel/arch/aarch64/arch.h over that port's
 * struct trap_frame, and is only stated separately because the portable core
 * is not compiled for that architecture yet.
 */

#if defined(__x86_64__)

#define SYSCALL_NR(frame)   ((frame)->rax)
#define SYSCALL_RET(frame)  ((frame)->rax)
#define SYSCALL_ARG0(frame) ((frame)->rdi)
#define SYSCALL_ARG1(frame) ((frame)->rsi)
#define SYSCALL_ARG2(frame) ((frame)->rdx)
#define SYSCALL_ARG3(frame) ((frame)->r10)
#define SYSCALL_ARG4(frame) ((frame)->r8)
#define SYSCALL_ARG5(frame) ((frame)->r9)

#endif

#endif
