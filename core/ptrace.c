#include <stddef.h>
#include <stdint.h>
#include <tunix/errno.h>
#include <tunix/kstring.h>
#include <tunix/process.h>
#include <tunix/ptrace.h>
#include <tunix/signal.h>
#include <tunix/usercopy.h>

static int64_t peek(struct process *tracee, uint64_t address, uint64_t data) {
    uint64_t word;
    int status = process_ptrace_access(tracee, address, &word, sizeof(word), 0);
    if (status != 0) return status;
    return copy_to_user(data, &word, sizeof(word)) != 0 ? -EFAULT : 0;
}

struct ptrace_iovec {
    uint64_t base;
    uint64_t length;
};

static int64_t regset(struct process *tracee, unsigned type, uint64_t iov_user, int write) {
    struct ptrace_iovec iov;
    if (copy_from_user(&iov, iov_user, sizeof(iov)) != 0) return -EFAULT;
    uint8_t buffer[PTRACE_REGSET_MAX];
    size_t size = iov.length < sizeof(buffer) ? (size_t)iov.length : sizeof(buffer);
    if (write && copy_from_user(buffer, iov.base, size) != 0) return -EFAULT;
    int status = process_ptrace_regs(tracee, type, buffer, &size, write);
    if (status != 0) return status;
    if (!write && copy_to_user(iov.base, buffer, size) != 0) return -EFAULT;
    iov.length = size;
    return copy_to_user(iov_user, &iov, sizeof(iov)) != 0 ? -EFAULT : 0;
}

static int64_t siginfo(struct process *tracee, uint64_t user, int write) {
    uint8_t bytes[SIGNAL_SIGINFO_SIZE] = {0};
    struct signal_info info;
    if (write) {
        if (copy_from_user(&info, user, sizeof(info)) != 0) return -EFAULT;
        return process_ptrace_siginfo(tracee, &info, 1);
    }
    int status = process_ptrace_siginfo(tracee, &info, 0);
    if (status != 0) return status;
    memcpy(bytes, &info, sizeof(info));
    return copy_to_user(user, bytes, sizeof(bytes)) != 0 ? -EFAULT : 0;
}

#if defined(__x86_64__)
#define USER_REGS_BYTES      216
#define USER_DEBUGREG_OFFSET 848
#define USER_DEBUGREG_BYTES  64

static int64_t regs_whole(struct process *tracee, unsigned type, uint64_t user, size_t size,
                          int write) {
    uint8_t buffer[PTRACE_REGSET_MAX];
    if (write && copy_from_user(buffer, user, size) != 0) return -EFAULT;
    int status = process_ptrace_regs(tracee, type, buffer, &size, write);
    if (status != 0) return status;
    return !write && copy_to_user(user, buffer, size) != 0 ? -EFAULT : 0;
}

static int64_t user_area(struct process *tracee, uint64_t offset, uint64_t data, int write) {
    if (offset & 7U) return -EIO;
    if (offset >= USER_DEBUGREG_OFFSET && offset < USER_DEBUGREG_OFFSET + USER_DEBUGREG_BYTES) {
        uint64_t zero = 0;
        if (write) return data ? -EIO : 0;
        return copy_to_user(data, &zero, sizeof(zero)) != 0 ? -EFAULT : 0;
    }
    if (offset >= USER_REGS_BYTES) return -EIO;
    uint8_t regs[USER_REGS_BYTES];
    size_t size = sizeof(regs);
    int status = process_ptrace_regs(tracee, NT_PRSTATUS, regs, &size, 0);
    if (status != 0) return status;
    if (!write) return copy_to_user(data, regs + offset, 8) != 0 ? -EFAULT : 0;
    memcpy(regs + offset, &data, 8);
    return process_ptrace_regs(tracee, NT_PRSTATUS, regs, &size, 1);
}
#endif

static int64_t request_on(struct process *tracee, int64_t request, uint64_t address,
                          uint64_t data) {
    switch (request) {
    case PTRACE_PEEKTEXT:
    case PTRACE_PEEKDATA: return peek(tracee, address, data);
    case PTRACE_POKETEXT:
    case PTRACE_POKEDATA: return process_ptrace_access(tracee, address, &data, sizeof(data), 1);
    case PTRACE_GETREGSET:  return regset(tracee, (unsigned)address, data, 0);
    case PTRACE_SETREGSET:  return regset(tracee, (unsigned)address, data, 1);
    case PTRACE_GETSIGINFO: return siginfo(tracee, data, 0);
    case PTRACE_SETSIGINFO: return siginfo(tracee, data, 1);
#if defined(__x86_64__)
    case PTRACE_GETREGS:   return regs_whole(tracee, NT_PRSTATUS, data, USER_REGS_BYTES, 0);
    case PTRACE_SETREGS:   return regs_whole(tracee, NT_PRSTATUS, data, USER_REGS_BYTES, 1);
    case PTRACE_GETFPREGS: return regs_whole(tracee, NT_PRFPREG, data, 512, 0);
    case PTRACE_SETFPREGS: return regs_whole(tracee, NT_PRFPREG, data, 512, 1);
    case PTRACE_PEEKUSER:  return user_area(tracee, address, data, 0);
    case PTRACE_POKEUSER:  return user_area(tracee, address, data, 1);
#endif
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
