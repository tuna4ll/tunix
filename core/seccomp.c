#include <stddef.h>
#include <stdint.h>
#include <tunix/errno.h>
#include <tunix/heap.h>
#include <tunix/process.h>
#include <tunix/seccomp.h>
#include <tunix/syscall_abi.h>
#include <tunix/usercopy.h>

#if defined(__x86_64__)
#define SECCOMP_AUDIT_ARCH AUDIT_ARCH_X86_64
#else
#define SECCOMP_AUDIT_ARCH AUDIT_ARCH_AARCH64
#endif

uint32_t bpf_run(const struct sock_filter *program, uint16_t length,
                 const struct seccomp_data *data) {
    const uint8_t *bytes = (const uint8_t *)data;
    uint32_t a = 0, x = 0;
    uint32_t mem[BPF_MEMWORDS] = {0};
    for (uint16_t pc = 0; pc < length; pc++) {
        const struct sock_filter *op = &program[pc];
        uint32_t k = op->k;
        switch (op->code) {
        case BPF_LD | BPF_W | BPF_ABS:
            if (k > sizeof(*data) - 4U || (k & 3U)) return SECCOMP_RET_KILL_PROCESS;
            a = *(const uint32_t *)(bytes + k);
            break;
        case BPF_LD | BPF_W | BPF_LEN: a = (uint32_t)sizeof(*data); break;
        case BPF_LD | BPF_IMM:         a = k; break;
        case BPF_LD | BPF_MEM:
            if (k >= BPF_MEMWORDS) return SECCOMP_RET_KILL_PROCESS;
            a = mem[k];
            break;
        case BPF_LDX | BPF_W | BPF_LEN: x = (uint32_t)sizeof(*data); break;
        case BPF_LDX | BPF_IMM:         x = k; break;
        case BPF_LDX | BPF_MEM:
            if (k >= BPF_MEMWORDS) return SECCOMP_RET_KILL_PROCESS;
            x = mem[k];
            break;
        case BPF_ST:
            if (k >= BPF_MEMWORDS) return SECCOMP_RET_KILL_PROCESS;
            mem[k] = a;
            break;
        case BPF_STX:
            if (k >= BPF_MEMWORDS) return SECCOMP_RET_KILL_PROCESS;
            mem[k] = x;
            break;
        case BPF_ALU | BPF_ADD | BPF_K:  a += k; break;
        case BPF_ALU | BPF_SUB | BPF_K:  a -= k; break;
        case BPF_ALU | BPF_MUL | BPF_K:  a *= k; break;
        case BPF_ALU | BPF_DIV | BPF_K:  a = k ? a / k : 0; break;
        case BPF_ALU | BPF_MOD | BPF_K:  a = k ? a % k : 0; break;
        case BPF_ALU | BPF_AND | BPF_K:  a &= k; break;
        case BPF_ALU | BPF_OR | BPF_K:   a |= k; break;
        case BPF_ALU | BPF_XOR | BPF_K:  a ^= k; break;
        case BPF_ALU | BPF_LSH | BPF_K:  a = k < 32U ? a << k : 0; break;
        case BPF_ALU | BPF_RSH | BPF_K:  a = k < 32U ? a >> k : 0; break;
        case BPF_ALU | BPF_ADD | BPF_X:  a += x; break;
        case BPF_ALU | BPF_SUB | BPF_X:  a -= x; break;
        case BPF_ALU | BPF_MUL | BPF_X:  a *= x; break;
        case BPF_ALU | BPF_DIV | BPF_X:  a = x ? a / x : 0; break;
        case BPF_ALU | BPF_MOD | BPF_X:  a = x ? a % x : 0; break;
        case BPF_ALU | BPF_AND | BPF_X:  a &= x; break;
        case BPF_ALU | BPF_OR | BPF_X:   a |= x; break;
        case BPF_ALU | BPF_XOR | BPF_X:  a ^= x; break;
        case BPF_ALU | BPF_LSH | BPF_X:  a = x < 32U ? a << x : 0; break;
        case BPF_ALU | BPF_RSH | BPF_X:  a = x < 32U ? a >> x : 0; break;
        case BPF_ALU | BPF_NEG:          a = (uint32_t)(-(int32_t)a); break;
        case BPF_JMP | BPF_JA:           pc += (uint16_t)k; break;
        case BPF_JMP | BPF_JEQ | BPF_K:  pc += a == k ? op->jt : op->jf; break;
        case BPF_JMP | BPF_JGT | BPF_K:  pc += a > k ? op->jt : op->jf; break;
        case BPF_JMP | BPF_JGE | BPF_K:  pc += a >= k ? op->jt : op->jf; break;
        case BPF_JMP | BPF_JSET | BPF_K: pc += (a & k) ? op->jt : op->jf; break;
        case BPF_JMP | BPF_JEQ | BPF_X:  pc += a == x ? op->jt : op->jf; break;
        case BPF_JMP | BPF_JGT | BPF_X:  pc += a > x ? op->jt : op->jf; break;
        case BPF_JMP | BPF_JGE | BPF_X:  pc += a >= x ? op->jt : op->jf; break;
        case BPF_JMP | BPF_JSET | BPF_X: pc += (a & x) ? op->jt : op->jf; break;
        case BPF_RET | BPF_K:            return k;
        case BPF_RET | BPF_A:            return a;
        case BPF_MISC | BPF_TAX:         x = a; break;
        case BPF_MISC | BPF_TXA:         a = x; break;
        default:                         return SECCOMP_RET_KILL_PROCESS;
        }
    }
    return SECCOMP_RET_KILL_PROCESS;
}

static int valid_opcode(uint16_t code) {
    switch (code) {
    case BPF_LD | BPF_W | BPF_ABS:
    case BPF_LD | BPF_W | BPF_LEN:
    case BPF_LD | BPF_IMM:
    case BPF_LD | BPF_MEM:
    case BPF_LDX | BPF_W | BPF_LEN:
    case BPF_LDX | BPF_IMM:
    case BPF_LDX | BPF_MEM:
    case BPF_ST:
    case BPF_STX:
    case BPF_ALU | BPF_ADD | BPF_K:
    case BPF_ALU | BPF_SUB | BPF_K:
    case BPF_ALU | BPF_MUL | BPF_K:
    case BPF_ALU | BPF_DIV | BPF_K:
    case BPF_ALU | BPF_MOD | BPF_K:
    case BPF_ALU | BPF_AND | BPF_K:
    case BPF_ALU | BPF_OR | BPF_K:
    case BPF_ALU | BPF_XOR | BPF_K:
    case BPF_ALU | BPF_LSH | BPF_K:
    case BPF_ALU | BPF_RSH | BPF_K:
    case BPF_ALU | BPF_ADD | BPF_X:
    case BPF_ALU | BPF_SUB | BPF_X:
    case BPF_ALU | BPF_MUL | BPF_X:
    case BPF_ALU | BPF_DIV | BPF_X:
    case BPF_ALU | BPF_MOD | BPF_X:
    case BPF_ALU | BPF_AND | BPF_X:
    case BPF_ALU | BPF_OR | BPF_X:
    case BPF_ALU | BPF_XOR | BPF_X:
    case BPF_ALU | BPF_LSH | BPF_X:
    case BPF_ALU | BPF_RSH | BPF_X:
    case BPF_ALU | BPF_NEG:
    case BPF_JMP | BPF_JA:
    case BPF_JMP | BPF_JEQ | BPF_K:
    case BPF_JMP | BPF_JGT | BPF_K:
    case BPF_JMP | BPF_JGE | BPF_K:
    case BPF_JMP | BPF_JSET | BPF_K:
    case BPF_JMP | BPF_JEQ | BPF_X:
    case BPF_JMP | BPF_JGT | BPF_X:
    case BPF_JMP | BPF_JGE | BPF_X:
    case BPF_JMP | BPF_JSET | BPF_X:
    case BPF_RET | BPF_K:
    case BPF_RET | BPF_A:
    case BPF_MISC | BPF_TAX:
    case BPF_MISC | BPF_TXA:         return 1;
    default:                         return 0;
    }
}

int bpf_check(const struct sock_filter *program, uint16_t length) {
    if (length == 0 || length > BPF_MAXINSNS) return -EINVAL;
    for (uint16_t pc = 0; pc < length; pc++) {
        const struct sock_filter *op = &program[pc];
        if (!valid_opcode(op->code)) return -EINVAL;
        if (BPF_CLASS(op->code) == BPF_JMP) {
            if (op->code == (BPF_JMP | BPF_JA)) {
                if (op->k >= (uint32_t)(length - pc - 1)) return -EINVAL;
            } else {
                if (op->jt >= length - pc - 1 || op->jf >= length - pc - 1) return -EINVAL;
            }
        }
        if ((BPF_CLASS(op->code) == BPF_LD || BPF_CLASS(op->code) == BPF_LDX ||
             op->code == BPF_ST || op->code == BPF_STX) &&
            BPF_MODE(op->code) == BPF_MEM && op->k >= BPF_MEMWORDS)
            return -EINVAL;
    }
    if (BPF_CLASS(program[length - 1].code) != BPF_RET) return -EINVAL;
    return 0;
}

static struct seccomp_filter *filter_copy_from_user(uint64_t user_program) {
    struct sock_fprog prog;
    if (copy_from_user(&prog, user_program, sizeof(prog)) != 0) return NULL;
    if (prog.len == 0 || prog.len > BPF_MAXINSNS) return NULL;
    uint64_t bytes = (uint64_t)prog.len * sizeof(struct sock_filter);
    struct seccomp_filter *filter = kmalloc(sizeof(*filter) + bytes);
    if (!filter) return NULL;
    if (copy_from_user(filter->program, prog.filter, bytes) != 0) {
        kfree(filter);
        return NULL;
    }
    if (bpf_check(filter->program, prog.len) != 0) {
        kfree(filter);
        return NULL;
    }
    filter->refs = 1;
    filter->length = prog.len;
    filter->prev = NULL;
    return filter;
}

int seccomp_active(const struct process *process) {
    return process && process->seccomp_mode != SECCOMP_MODE_DISABLED;
}

static int64_t attach_filter(struct process *process, struct seccomp_filter *filter) {
    if (process->seccomp_mode == SECCOMP_MODE_STRICT) {
        kfree(filter);
        return -EINVAL;
    }
    filter->prev = process->seccomp_filters;
    process->seccomp_filters = filter;
    process->seccomp_mode = SECCOMP_MODE_FILTER;
    return 0;
}

int64_t seccomp_set_mode_filter(uint32_t flags, uint64_t user_program) {
    struct process *process = process_current();
    if (!process) return -EINVAL;
    if (flags &
        ~(SECCOMP_FILTER_FLAG_TSYNC | SECCOMP_FILTER_FLAG_LOG | SECCOMP_FILTER_FLAG_SPEC_ALLOW |
          SECCOMP_FILTER_FLAG_TSYNC_ESRCH))
        return -EINVAL;
    if (!process->no_new_privs && process->cred.euid != 0) return -EACCES;
    struct seccomp_filter *filter = filter_copy_from_user(user_program);
    if (!filter) return -EINVAL;
    return attach_filter(process, filter);
}

int64_t seccomp_set_mode_strict(void) {
    struct process *process = process_current();
    if (!process) return -EINVAL;
    if (process->seccomp_mode == SECCOMP_MODE_FILTER) return -EINVAL;
    process->seccomp_mode = SECCOMP_MODE_STRICT;
    return 0;
}

int64_t seccomp_prctl_set(uint64_t mode, uint64_t user_program) {
    if (mode == SECCOMP_MODE_STRICT) return seccomp_set_mode_strict();
    if (mode == SECCOMP_MODE_FILTER) return seccomp_set_mode_filter(0, user_program);
    return -EINVAL;
}

static int strict_allows(uint64_t nr) {
#if defined(__x86_64__)
    return nr == 0 || nr == 1 || nr == 60 || nr == 231 || nr == 15;
#else
    return nr == 63 || nr == 64 || nr == 93 || nr == 94 || nr == 139;
#endif
}

uint32_t seccomp_decide(struct syscall_frame *frame, uint64_t native_nr) {
    struct process *process = process_current();
    if (!process || process->seccomp_mode == SECCOMP_MODE_DISABLED) return SECCOMP_RET_ALLOW;

    if (process->seccomp_mode == SECCOMP_MODE_STRICT)
        return strict_allows(native_nr) ? SECCOMP_RET_ALLOW : SECCOMP_RET_KILL_PROCESS;

    struct seccomp_data data = {
        .nr = (int32_t)native_nr,
        .arch = SECCOMP_AUDIT_ARCH,
        .instruction_pointer = SYSCALL_IP(frame),
        .args = {SYSCALL_ARG0(frame), SYSCALL_ARG1(frame), SYSCALL_ARG2(frame), SYSCALL_ARG3(frame),
                 SYSCALL_ARG4(frame), SYSCALL_ARG5(frame)},
    };

    uint32_t best = SECCOMP_RET_ALLOW;
    int best_rank = 0;
    for (struct seccomp_filter *filter = process->seccomp_filters; filter; filter = filter->prev) {
        uint32_t result = bpf_run(filter->program, filter->length, &data);
        int rank;
        switch (result & SECCOMP_RET_ACTION_FULL) {
        case SECCOMP_RET_KILL_PROCESS: rank = 7; break;
        case SECCOMP_RET_KILL_THREAD:  rank = 6; break;
        case SECCOMP_RET_TRAP:         rank = 5; break;
        case SECCOMP_RET_ERRNO:        rank = 4; break;
        case SECCOMP_RET_USER_NOTIF:   rank = 3; break;
        case SECCOMP_RET_TRACE:        rank = 2; break;
        case SECCOMP_RET_LOG:          rank = 1; break;
        case SECCOMP_RET_ALLOW:        rank = 0; break;
        default:                       rank = 6; break;
        }
        if (rank > best_rank) {
            best_rank = rank;
            best = result;
        }
    }
    return best;
}

void seccomp_inherit(struct process *child, const struct process *parent) {
    child->seccomp_mode = parent->seccomp_mode;
    child->seccomp_filters = parent->seccomp_filters;
    for (struct seccomp_filter *filter = child->seccomp_filters; filter; filter = filter->prev)
        __atomic_add_fetch(&filter->refs, 1, __ATOMIC_RELAXED);
}

void seccomp_release(struct process *process) {
    struct seccomp_filter *filter = process->seccomp_filters;
    process->seccomp_filters = NULL;
    process->seccomp_mode = SECCOMP_MODE_DISABLED;
    while (filter) {
        struct seccomp_filter *prev = filter->prev;
        if (__atomic_sub_fetch(&filter->refs, 1, __ATOMIC_ACQ_REL) == 0) kfree(filter);
        filter = prev;
    }
}
