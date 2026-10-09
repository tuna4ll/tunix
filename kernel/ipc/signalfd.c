#include <stddef.h>
#include <stdint.h>
#include <tunix/heap.h>
#include <tunix/kstring.h>
#include <tunix/process.h>
#include <tunix/signalfd.h>

#define EAGAIN 11
#define EINVAL 22

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
    uint64_t undeliverable = signal_bit(9) | signal_bit(19);
    return __atomic_load_n(&process->signal_pending, __ATOMIC_ACQUIRE) & context->mask &
        ~undeliverable;
}

int signalfd_read_ready(struct signalfd_context *context) {
    return available_signals(context) != 0;
}

int64_t signalfd_read(struct signalfd_context *context, size_t size, void *buffer) {
    if (!context || !buffer) return -EINVAL;
    if (size < sizeof(struct signalfd_siginfo)) return -EINVAL;

    struct process *process = process_current();
    if (!process) return -EINVAL;

    size_t capacity = size / sizeof(struct signalfd_siginfo);
    struct signalfd_siginfo *out = (struct signalfd_siginfo *)buffer;
    size_t produced = 0;

    for (int signal_number = 1; signal_number <= TUNIX_NSIG && produced < capacity;
         signal_number++) {
        uint64_t bit = signal_bit(signal_number);
        if (!(available_signals(context) & bit)) continue;

        __atomic_fetch_and(&process->signal_pending, ~bit, __ATOMIC_ACQ_REL);
        memset(&out[produced], 0, sizeof(out[produced]));
        out[produced].ssi_signo = (uint32_t)signal_number;

        if (process->signal_user_sent & bit) {
            out[produced].ssi_pid = process->signal_sender_pid[signal_number - 1];
            out[produced].ssi_uid = process->signal_sender_uid[signal_number - 1];
            out[produced].ssi_code = process->signal_sender_code[signal_number - 1];
            process->signal_user_sent &= ~bit;
            process->signal_sender_pid[signal_number - 1] = 0;
            process->signal_sender_uid[signal_number - 1] = 0;
        }
        produced++;
    }

    if (!produced) return -EAGAIN;
    return (int64_t)(produced * sizeof(struct signalfd_siginfo));
}
