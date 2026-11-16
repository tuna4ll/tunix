#ifndef TUNIX_PTRACE_H
#define TUNIX_PTRACE_H

#include <stddef.h>
#include <stdint.h>

#define PTRACE_TRACEME   0
#define PTRACE_PEEKTEXT  1
#define PTRACE_PEEKDATA  2
#define PTRACE_PEEKUSER  3
#define PTRACE_POKETEXT  4
#define PTRACE_POKEDATA  5
#define PTRACE_POKEUSER  6
#define PTRACE_CONT      7
#define PTRACE_KILL      8
#define PTRACE_SINGLESTEP 9
#define PTRACE_GETREGS   12
#define PTRACE_SETREGS   13
#define PTRACE_GETFPREGS 14
#define PTRACE_SETFPREGS 15
#define PTRACE_ATTACH    16
#define PTRACE_DETACH    17
#define PTRACE_SYSCALL   24
#define PTRACE_SETOPTIONS 0x4200
#define PTRACE_GETSIGINFO 0x4202
#define PTRACE_SETSIGINFO 0x4203
#define PTRACE_GETREGSET  0x4204
#define PTRACE_SETREGSET  0x4205
#define PTRACE_SEIZE      0x4206

#define NT_PRSTATUS 1
#define NT_PRFPREG  2
#define NT_ARM_TLS  0x401
#define NT_ARM_SYSTEM_CALL 0x404

#define PTRACE_O_TRACESYSGOOD 0x01
#define PTRACE_O_MASK         PTRACE_O_TRACESYSGOOD

#define PTRACE_REGSET_MAX 528

struct process;
struct syscall_frame;

int arch_ptrace_get_regs(const struct process *tracee, const struct syscall_frame *frame,
                         const uint8_t *fpu, unsigned type, uint8_t *out, size_t *size);
int arch_ptrace_set_regs(struct process *tracee, struct syscall_frame *frame, uint8_t *fpu,
                         unsigned type, const uint8_t *in);

int64_t sys_ptrace(int64_t request, int64_t pid, uint64_t address, uint64_t data);

#endif
