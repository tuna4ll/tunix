#include <stddef.h>
#include <stdint.h>
#include <tunix/errno.h>
#include <tunix/process.h>
#include <tunix/ptrace.h>
#include <tunix/usercopy.h>

static int64_t peek(struct process *tracee, uint64_t address, uint64_t data) {
    uint64_t word;
    int status = process_ptrace_access(tracee, address, &word, sizeof(word), 0);
    if (status != 0) return status;
    return copy_to_user(data, &word, sizeof(word)) != 0 ? -EFAULT : 0;
}

static int64_t request_on(struct process *tracee, int64_t request, uint64_t address,
                          uint64_t data) {
    switch (request) {
    case PTRACE_PEEKTEXT:
    case PTRACE_PEEKDATA: return peek(tracee, address, data);
    case PTRACE_POKETEXT:
    case PTRACE_POKEDATA: return process_ptrace_access(tracee, address, &data, sizeof(data), 1);
    case PTRACE_CONT:     return process_ptrace_resume(tracee, (int)data, 0);
    case PTRACE_DETACH:   return process_ptrace_resume(tracee, (int)data, 1);
    case PTRACE_KILL:     return process_ptrace_resume(tracee, 0, 2);
    default:              return -EIO;
    }
}

int64_t sys_ptrace(int64_t request, int64_t pid, uint64_t address, uint64_t data) {
    if (request == PTRACE_TRACEME) return process_ptrace_traceme();
    if (pid <= 0) return -ESRCH;
    if (request == PTRACE_ATTACH) return process_ptrace_attach((uint64_t)pid, 0, 0);
    if (request == PTRACE_SEIZE) return address ? -EIO : process_ptrace_attach((uint64_t)pid, 1, 0);
    int error = -ESRCH;
    struct process *tracee = process_ptrace_get((uint64_t)pid, request != PTRACE_KILL, &error);
    if (!tracee) return error;
    int64_t result = request_on(tracee, request, address, data);
    process_put(tracee);
    return result;
}
