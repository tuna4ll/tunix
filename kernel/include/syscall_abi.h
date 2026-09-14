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
 * SYSCALL_REWIND backs the saved program counter over the trap instruction so
 * an interrupted call runs again when the process resumes.  It is a separate
 * operation rather than arithmetic at the call sites because the instruction
 * is two bytes on x86-64 (`syscall`) and four on AArch64 (`svc #0`).
 *
 * The frame each of these reaches into is defined per architecture in
 * syscall.h.
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

#define SYSCALL_USER_SP(frame) ((frame)->user_rsp)
#define SYSCALL_REWIND(frame)  ((frame)->user_rip -= 2U)

#elif defined(__aarch64__)

#define SYSCALL_NR(frame)   ((frame)->x[8])
#define SYSCALL_RET(frame)  ((frame)->x[0])
#define SYSCALL_ARG0(frame) ((frame)->x[0])
#define SYSCALL_ARG1(frame) ((frame)->x[1])
#define SYSCALL_ARG2(frame) ((frame)->x[2])
#define SYSCALL_ARG3(frame) ((frame)->x[3])
#define SYSCALL_ARG4(frame) ((frame)->x[4])
#define SYSCALL_ARG5(frame) ((frame)->x[5])

#define SYSCALL_USER_SP(frame) ((frame)->sp_el0)
#define SYSCALL_REWIND(frame)  ((frame)->elr -= 4U)

#else
#error "no syscall ABI mapping for this architecture"
#endif

#endif
