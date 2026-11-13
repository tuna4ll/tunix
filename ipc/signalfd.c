#include <stddef.h>
#include <stdint.h>
#include <tunix/heap.h>
#include <tunix/kstring.h>
#include <tunix/process.h>
#include <tunix/signalfd.h>
#include <tunix/errno.h>

struct signalfd_siginfo {
    uint32_t ssi_signo;
    int32_t ssi_errno;
    int32_t ssi_code;
    uint32_t ssi_pid;
    uint32_t ssi_uid;
    int32_t ssi_fd;
    uint32_t ssi_tid;
    uint32_t ssi_band;
    uint32_t ssi_overrun;
    uint32_t ssi_trapno;
    int32_t ssi_status;
    int32_t ssi_int;
    uint64_t ssi_ptr;
    uint64_t ssi_utime;
    uint64_t ssi_stime;
    uint64_t ssi_addr;
    uint16_t ssi_addr_lsb;
    uint16_t __pad2;
    int32_t ssi_syscall;
    uint64_t ssi_call_addr;
    uint32_t ssi_arch;
    uint8_t __pad[28];
};

typedef char signalfd_siginfo_size_check[(sizeof(struct signalfd_siginfo) == 128) ? 1 : -1];

struct signalfd_context {
    uint64_t mask;
};

static uint64_t signal_bit(int signal_number) {
    if (signal_number < 1 || signal_number > 64) return 0;
    return 1ULL << (signal_number - 1);
}

struct signalfd_context *signalfd_create(uint64_t mask) {
    struct signalfd_context *context = (struct signalfd_context *)kmalloc(sizeof(*context));
    if (!context) return NULL;
    context->mask = mask;
    return context;
}

void signalfd_destroy(struct signalfd_context *context) { kfree(context); }

void signalfd_set_mask(struct signalfd_context *context, uint64_t mask) {
    if (context) context->mask = mask;
}

static uint64_t available_signals(struct signalfd_context *context) {
    struct process *process = process_poll_subject();
    if (!context || !process) return 0;
    uint64_t undeliverable = signal_bit(SIGKILL) | signal_bit(SIGSTOP);
    return __atomic_load_n(&process->signal_pending, __ATOMIC_ACQUIRE) & context->mask &
        ~undeliverable;
}

int signalfd_read_ready(struct signalfd_context *context) {
    return available_signals(context) != 0;
}

int64_t signalfd_read(struct signalfd_context *context, size_t size, void *buffer) {
    if (!context || !buffer) return -EINVAL;
    if (size < sizeof(struct signalfd_siginfo)) return -EINVAL;
    if (!process_current()) return -EINVAL;

    size_t capacity = size / sizeof(struct signalfd_siginfo);
    struct signalfd_siginfo *out = (struct signalfd_siginfo *)buffer;
    uint64_t wanted = context->mask & ~(signal_bit(SIGKILL) | signal_bit(SIGSTOP));
    size_t produced = 0;

    while (produced < capacity) {
        struct signal_info info;
        if (!process_take_signal(wanted, &info)) break;
        struct signalfd_siginfo *entry = &out[produced++];
        memset(entry, 0, sizeof(*entry));
        entry->ssi_signo = (uint32_t)info.signo;
        entry->ssi_errno = info.error;
        entry->ssi_code = info.code;
        int kernel_code = info.code > 0 && info.code != SI_KERNEL;
        if (info.code == SI_TIMER) {
            entry->ssi_tid = (uint32_t)info.fields.timer.id;
            entry->ssi_overrun = (uint32_t)info.fields.timer.overrun;
            entry->ssi_int = (int32_t)info.fields.timer.value;
            entry->ssi_ptr = info.fields.timer.value;
        } else if (kernel_code && info.signo == SIGCHLD) {
            entry->ssi_pid = (uint32_t)info.fields.child.pid;
            entry->ssi_uid = info.fields.child.uid;
            entry->ssi_status = info.fields.child.status;
            entry->ssi_utime = (uint64_t)info.fields.child.utime;
            entry->ssi_stime = (uint64_t)info.fields.child.stime;
        } else if (kernel_code &&
                   (info.signo == SIGSEGV || info.signo == SIGBUS || info.signo == SIGILL ||
                    info.signo == SIGFPE || info.signo == SIGTRAP)) {
            entry->ssi_addr = info.fields.fault.address;
        } else {
            entry->ssi_pid = (uint32_t)info.fields.sender.pid;
            entry->ssi_uid = info.fields.sender.uid;
            entry->ssi_int = (int32_t)info.fields.sender.value;
            entry->ssi_ptr = info.fields.sender.value;
        }
    }

    if (!produced) return -EAGAIN;
    return (int64_t)(produced * sizeof(struct signalfd_siginfo));
}
