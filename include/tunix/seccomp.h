#ifndef TUNIX_SECCOMP_H
#define TUNIX_SECCOMP_H

#include <stdint.h>
#include <uapi/seccomp.h>

struct process;
struct syscall_frame;

struct seccomp_filter {
    struct seccomp_filter *prev;
    uint32_t refs;
    uint16_t length;
    struct sock_filter program[];
};

uint32_t bpf_run(const struct sock_filter *program, uint16_t length,
                 const struct seccomp_data *data);
int bpf_check(const struct sock_filter *program, uint16_t length);

int64_t seccomp_set_mode_strict(void);
int64_t seccomp_set_mode_filter(uint32_t flags, uint64_t user_program);
int64_t seccomp_prctl_set(uint64_t mode, uint64_t user_program);
uint32_t seccomp_decide(struct syscall_frame *frame, uint64_t native_nr);
int seccomp_active(const struct process *process);
void seccomp_inherit(struct process *child, const struct process *parent);
void seccomp_release(struct process *process);

#endif
