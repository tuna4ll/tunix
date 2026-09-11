#ifndef TUNIX_PROCESS_H
#define TUNIX_PROCESS_H

#include <stdint.h>
#include "cred.h"
#include "file.h"
#include "signal.h"
#include "syscall.h"

#define PROCESS_MAX_FDS 256
#define PROCESS_FD_CLOEXEC 1U
#define PROCESS_READY 0
#define PROCESS_RUNNING 1
#define PROCESS_BLOCKED 2
#define PROCESS_ZOMBIE 3
#define PROCESS_DEAD 4
#define PROCESS_STOPPED 5

#define WNOHANG 1
#define WUNTRACED 2
#define WCONTINUED 8
#define WSTOPPED 2
#define WEXITED 4
#define WNOWAIT 0x01000000
#define WNOTHREAD 0x20000000
#define WALLCHILDREN 0x40000000
#define WCLONE 0x80000000

struct vfs_node;
struct pty_pair;
struct interrupt_frame;

#define VM_ANONYMOUS 0x1U
#define VM_FILE_PAGES 0x2U

struct vm_area {
    uint64_t start;
    uint64_t end;
    uint64_t page_flags;
    uint32_t kind;
    struct file *file;
    uint64_t offset;
    struct vm_area *next;
};

struct process_memory {
    uint64_t cr3;
    uint64_t refs;
    uint64_t brk_start;
    uint64_t brk_end;
    uint64_t mmap_base;
    struct vm_area *areas;
};

struct file_table {
    int refs;
    struct file *fds[PROCESS_MAX_FDS];
    uint8_t fd_flags[PROCESS_MAX_FDS];
};

#define PROCESS_FPU_STATE_SIZE 2560

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
    int termination_signal;
    int stop_signal;
    int stop_reported;
    int continued_pending;
    uint32_t umask;
    struct credentials cred;
    uint64_t cr3;
    struct process_memory *memory;
    int is_thread;
    int group_exit_pending;
    uint64_t entry;
    uint64_t user_stack_top;
    uint64_t kernel_stack_base;
    uint64_t kernel_stack_top;
    uint8_t fpu_state[PROCESS_FPU_STATE_SIZE + 64];
    uint64_t brk_start;
    uint64_t brk_end;
    uint64_t mmap_base;
    uint64_t fs_base;
    uint64_t gs_base;
    uint64_t clear_child_tid_user;
    uint64_t robust_list_head;
    uint64_t robust_list_length;
    uint64_t signal_stack_pointer;
    uint64_t signal_stack_size;
    int signal_stack_flags;
    int pdeath_signal;
    int dumpable;
    int no_new_privs;
    int keep_capabilities;
    int child_subreaper;
    int thp_disable;
    uint64_t timerslack_ns;
    uint64_t start_time_ns;
    uint64_t runtime_ns;
    uint64_t last_scheduled_ns;
    uint64_t virtual_runtime_ns;
    uint32_t time_slice_ticks;
    int policy;
    int rt_priority;
    int nice;
    uint64_t affinity_mask;
    uint64_t involuntary_switches;
    char cmdline[512];
    uint64_t cmdline_length;
    struct vfs_node *cwd;
    struct pty_pair *controlling_pty;
    struct syscall_frame saved_frame;
    struct file_table *files;

    int wait4_active;
    int64_t wait_pid;
    uint64_t wait_status_user;
    int wait_options;

    int io_wait_active;
    uint64_t io_wait_syscall;
    uint64_t io_wait_deadline_ns;

    int syscall_rewound;

    int futex_wait_active;
    uint64_t futex_wait_address;
    uint64_t futex_wait_key;
    uint32_t futex_wait_expected;
    uint32_t futex_wait_bitset;
    const void *wait_channel;
    uint64_t futex_wait_deadline_ns;

    uint64_t itimer_real_interval_ns;
    uint64_t itimer_real_deadline_ns;

    uint64_t signal_pending;
    uint64_t signal_user_sent;
    uint32_t signal_sender_pid[TUNIX_NSIG];
    uint32_t signal_sender_uid[TUNIX_NSIG];
    uint64_t signal_blocked;
    uint64_t signal_saved_mask;
    uint64_t signal_wait_mask_saved;
    int signal_wait_mask_active;
    int in_signal;
    struct syscall_frame signal_saved_frame;
    uint64_t signal_context_address;
    struct tunix_sigaction signal_actions[TUNIX_NSIG];

    struct process *next;
};

void process_init(void);
struct process *process_create_from_path(const char *path);
struct process *process_current(void);
void process_dump_all(void);
struct process *process_find(uint64_t pid);

#define PROCESS_SCHED_OTHER 0
#define PROCESS_SCHED_FIFO 1
#define PROCESS_SCHED_RR 2
#define PROCESS_SCHED_BATCH 3
#define PROCESS_SCHED_IDLE 5
#define PROCESS_RT_PRIORITY_MAX 99
#define PROCESS_RT_PRIORITY_UNPRIVILEGED_MAX 20

int process_set_scheduler(uint64_t tid, int policy, int rt_priority);
int process_get_scheduler(uint64_t tid, int *policy, int *rt_priority);
int process_set_nice(uint64_t tid, int nice);
int process_get_nice(uint64_t tid, int *nice);
int process_set_affinity(uint64_t tid, uint64_t mask);
int process_get_affinity(uint64_t tid, uint64_t *mask);
uint64_t process_current_pid(void);
uint64_t process_current_tid(void);
uint64_t process_current_ppid(void);
void process_start_first(void) __attribute__((noreturn));
void process_run_idle(void) __attribute__((noreturn));
void process_yield_from_syscall(struct syscall_frame *frame);
void process_timer_interrupt(struct interrupt_frame *frame);
int process_map_area(uint64_t start, uint64_t end, uint64_t page_flags,
                     uint32_t kind, struct file *file, uint64_t offset);
void process_unmap_area(uint64_t start, uint64_t end);
void process_protect_area(uint64_t start, uint64_t end, uint64_t page_flags);
int process_area_range_free(uint64_t start, uint64_t end);
int process_find_free_range(uint64_t start, uint64_t length, uint64_t *base_out);
int process_commit_area(uint64_t fault_address);
struct vm_area *process_find_area(uint64_t address);
int process_sync_file_areas(uint64_t start, uint64_t end);

int process_grow_user_stack(uint64_t fault_address);
int process_handle_cow_fault(uint64_t fault_address);
void process_enable_extended_fpu(void);
int process_signal_has_handler(int signal_number);
int process_fault_from_interrupt(struct interrupt_frame *frame, int signal_number);
void process_run_child_first_from_syscall(struct syscall_frame *frame, uint64_t child_pid);
void process_reap_deferred(void);
void process_exit_from_syscall(struct syscall_frame *frame, int status);
void process_exit_group_from_syscall(struct syscall_frame *frame, int status);
int process_install_file(struct process *process, struct file *file, int minimum_fd);
int process_install_file_flags(struct process *process, struct file *file, int minimum_fd, uint8_t flags);
uint8_t process_get_fd_flags(const struct process *process, int fd);
int process_set_fd_flags(struct process *process, int fd, uint8_t flags);
int process_close_fd(struct process *process, int fd);
int64_t process_fork_from_syscall(struct syscall_frame *frame);
int64_t process_clone_thread_from_syscall(struct syscall_frame *frame,
                                          uint64_t child_stack, uint64_t tls,
                                          uint64_t parent_tid_user,
                                          uint64_t child_tid_user,
                                          uint64_t flags);
int64_t process_exec_from_syscall(struct syscall_frame *frame, const char *path,
                                  const char *const argv[], const char *const envp[],
                                  const struct vfs_node *credential_source);
int64_t process_waitpid_from_syscall(struct syscall_frame *frame, int64_t pid,
                                     uint64_t status_user, int options);
int64_t process_waitid_from_syscall(int64_t pid_spec, uint64_t info_user,
                                    int options);
int process_send_signal(int64_t pid, int signal_number);
int process_send_signal_checked(int64_t pid, int signal_number);
void process_set_sigaction(int signal_number, const struct tunix_sigaction *action);
int process_setpgid(int64_t pid, int64_t pgid);
int64_t process_setsid(void);
void process_prepare_user_return(struct syscall_frame *frame);

int process_signal_interrupts_wait(void);
void process_swap_signal_mask(uint64_t mask);
void process_restore_signal_mask(void);
int process_sigreturn(struct syscall_frame *frame);
#define FUTEX_BITSET_MATCH_ANY 0xFFFFFFFFU

int64_t process_futex_wait(struct syscall_frame *frame, uint64_t address,
                           uint32_t expected, int64_t timeout_ns,
                           uint32_t bitset, int shared);
int process_futex_wake(uint64_t address, int maximum, uint32_t bitset, int shared);

int process_sleep_on(struct syscall_frame *frame, const void *channel);
int process_wake_all(const void *channel);
const void *process_io_wait_channel(void);
int process_wake_io(void);
uint32_t process_get_umask(void);
uint32_t process_set_umask(uint32_t mask);
void process_set_fs_base(uint64_t value);
uint64_t process_get_fs_base(void);
void process_set_gs_base(uint64_t value);
uint64_t process_get_gs_base(void);
void process_account_runtime(void);
uint64_t process_runtime_ns(const struct process *process);
uint64_t process_count(void);
uint64_t process_created_count(void);
uint64_t process_runnable_count(void);
uint64_t process_blocked_count(void);
uint64_t process_total_runtime_ns(void);

#endif
