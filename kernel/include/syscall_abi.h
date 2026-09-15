#ifndef TUNIX_SYSCALL_ABI_H
#define TUNIX_SYSCALL_ABI_H

#if defined(__x86_64__)

#define SYSCALL_NR(frame)   ((frame)->rax)
#define SYSCALL_RET(frame)  ((frame)->rax)
#define SYSCALL_ARG0(frame) ((frame)->rdi)
#define SYSCALL_ARG1(frame) ((frame)->rsi)
#define SYSCALL_ARG2(frame) ((frame)->rdx)
#define SYSCALL_ARG3(frame) ((frame)->r10)
#define SYSCALL_ARG4(frame) ((frame)->r8)
#define SYSCALL_ARG5(frame) ((frame)->r9)
#define SYSCALL_CLONE_CHILD_TID(frame) SYSCALL_ARG3(frame)
#define SYSCALL_CLONE_TLS(frame) SYSCALL_ARG4(frame)

#define SYSCALL_USER_SP(frame) ((frame)->user_rsp)
#define SYSCALL_REWIND(frame)  ((frame)->user_rip -= 2U)
#define SYSCALL_ADVANCE(frame) ((frame)->user_rip += 2U)
#define SYSCALL_IP(frame)      ((frame)->user_rip)
#define SYSCALL_RESTART(frame, number) (SYSCALL_REWIND(frame), SYSCALL_RET(frame) = (number))

#elif defined(__aarch64__)

#define SYSCALL_NR(frame)   ((frame)->reserved[0])
#define SYSCALL_RET(frame)  ((frame)->x[0])
#define SYSCALL_ARG0(frame) ((frame)->x[0])
#define SYSCALL_ARG1(frame) ((frame)->x[1])
#define SYSCALL_ARG2(frame) ((frame)->x[2])
#define SYSCALL_ARG3(frame) ((frame)->x[3])
#define SYSCALL_ARG4(frame) ((frame)->x[4])
#define SYSCALL_ARG5(frame) ((frame)->x[5])
#define SYSCALL_CLONE_CHILD_TID(frame) SYSCALL_ARG4(frame)
#define SYSCALL_CLONE_TLS(frame) SYSCALL_ARG3(frame)

#define SYSCALL_USER_SP(frame) ((frame)->sp_el0)
#define SYSCALL_REWIND(frame)  ((frame)->elr -= 4U)
#define SYSCALL_ADVANCE(frame) ((frame)->elr += 4U)
#define SYSCALL_IP(frame)      ((frame)->elr)
#define SYSCALL_RESTART(frame, number) (SYSCALL_REWIND(frame), SYSCALL_NR(frame) = (number))

#else
#error "no syscall ABI mapping for this architecture"
#endif

#endif
