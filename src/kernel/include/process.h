#ifndef TUNIX_PROCESS_H
#define TUNIX_PROCESS_H

#include <stdint.h>
#include "file.h"
#include "signal.h"
#include "syscall.h"

#define PROCESS_MAX_FDS 64
#define PROCESS_READY 0
#define PROCESS_RUNNING 1
#define PROCESS_BLOCKED 2
#define PROCESS_ZOMBIE 3
#define PROCESS_DEAD 4
#define PROCESS_STOPPED 5

#define WNOHANG 1
#define WUNTRACED 2
#define WCONTINUED 8

struct vfs_node;
struct pty_pair;

struct process_memory {
    uint64_t cr3;
    uint64_t refs;
    uint64_t brk_start;
    uint64_t brk_end;
    uint64_t mmap_base;
};

struct process {
    uint64_t pid;
    uint64_t tgid;
    uint64_t ppid;
    uint64_t pgid;
    uint64_t sid;
    char name[64];
    char exe_path[256];
    int state;
    int exit_status;
    int stop_signal;
    int stop_reported;
    int continued_pending;
    uint64_t cr3;
    struct process_memory *memory;
    int is_thread;
    uint64_t entry;
    uint64_t user_stack_top;
    uint64_t kernel_stack_base;
    uint64_t kernel_stack_top;
    uint64_t brk_start;
    uint64_t brk_end;
    uint64_t mmap_base;
    uint64_t fs_base;
    uint64_t clear_child_tid_user;
    uint64_t start_time_ns;
    uint64_t runtime_ns;
    uint64_t last_scheduled_ns;
    char cmdline[512];
    uint64_t cmdline_length;
    struct vfs_node *cwd;
    struct pty_pair *controlling_pty;
    struct syscall_frame saved_frame;
    struct file *fds[PROCESS_MAX_FDS];

    int64_t wait_pid;
    uint64_t wait_status_user;
    int wait_options;

    int io_wait_active;
    uint64_t io_wait_syscall;
    uint64_t io_wait_deadline_ns;

    int futex_wait_active;
    uint64_t futex_wait_address;
    uint64_t futex_wait_deadline_ns;

    uint64_t signal_pending;
    uint64_t signal_blocked;
    uint64_t signal_saved_mask;
    int in_signal;
    struct syscall_frame signal_saved_frame;
    struct tunix_sigaction signal_actions[TUNIX_NSIG];

    struct process *next;
};

void process_init(void);
struct process *process_create_from_path(const char *path);
struct process *process_current(void);
struct process *process_find(uint64_t pid);
uint64_t process_current_pid(void);
uint64_t process_current_tid(void);
uint64_t process_current_ppid(void);
void process_start_first(void) __attribute__((noreturn));
void process_yield_from_syscall(struct syscall_frame *frame);
void process_run_child_first_from_syscall(struct syscall_frame *frame, uint64_t child_pid);
void process_reap_deferred(void);
void process_exit_from_syscall(struct syscall_frame *frame, int status);
void process_exit_group_from_syscall(struct syscall_frame *frame, int status);
int process_install_file(struct process *process, struct file *file, int minimum_fd);
int process_close_fd(struct process *process, int fd);
int64_t process_fork_from_syscall(struct syscall_frame *frame);
int64_t process_clone_thread_from_syscall(struct syscall_frame *frame,
                                          uint64_t child_stack, uint64_t tls,
                                          uint64_t parent_tid_user,
                                          uint64_t child_tid_user,
                                          uint64_t flags);
int64_t process_exec_from_syscall(struct syscall_frame *frame, const char *path,
                                  const char *const argv[], const char *const envp[]);
int64_t process_waitpid_from_syscall(struct syscall_frame *frame, int64_t pid,
                                     uint64_t status_user, int options);
int process_send_signal(int64_t pid, int signal_number);
int process_setpgid(int64_t pid, int64_t pgid);
int64_t process_setsid(void);
void process_prepare_user_return(struct syscall_frame *frame);
int process_sigreturn(struct syscall_frame *frame);
int64_t process_futex_wait(struct syscall_frame *frame, uint64_t address,
                           uint32_t expected, int64_t timeout_ns);
int process_futex_wake(uint64_t address, int maximum);
void process_set_fs_base(uint64_t value);
uint64_t process_get_fs_base(void);
void process_account_runtime(void);
uint64_t process_runtime_ns(const struct process *process);
uint64_t process_count(void);
uint64_t process_created_count(void);
uint64_t process_runnable_count(void);
uint64_t process_blocked_count(void);
uint64_t process_total_runtime_ns(void);

#endif
