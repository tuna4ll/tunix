#ifndef TUNIX_PTRACE_H
#define TUNIX_PTRACE_H

#include <stdint.h>

#define PTRACE_TRACEME   0
#define PTRACE_PEEKTEXT  1
#define PTRACE_PEEKDATA  2
#define PTRACE_POKETEXT  4
#define PTRACE_POKEDATA  5
#define PTRACE_CONT      7
#define PTRACE_KILL      8
#define PTRACE_ATTACH    16
#define PTRACE_DETACH    17
#define PTRACE_SEIZE     0x4206

int64_t sys_ptrace(int64_t request, int64_t pid, uint64_t address, uint64_t data);

#endif
