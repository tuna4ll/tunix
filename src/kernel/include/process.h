#ifndef TUNIX_PROCESS_H
#define TUNIX_PROCESS_H

#include <stdint.h>
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

struct vfs_node;
struct pty_pair;
struct interrupt_frame;

/*
 * A MAP_SHARED mapping of a file, remembered so it can be grown later.
 *
 * Page tables alone cannot answer "what backs this address", which is exactly
 * what mremap() needs in order to extend a mapping over more of its object.
 * This is the minimum record that answers it; it is not general VMA tracking,
 * and only shared file mappings are listed.
 *
 * The reference on `file` is what keeps the backing object alive when the
 * process closes the descriptor but keeps the mapping.
 */
#define PROCESS_MAX_FILE_MAPPINGS 32

struct process_file_mapping {
    uint64_t start;   /* page-aligned user address, 0 when the slot is free */
    uint64_t length;  /* page-aligned byte count */
    uint64_t offset;  /* byte offset into the backing object */
    struct file *file;
};

struct process_memory {
    uint64_t cr3;
    uint64_t refs;
    uint64_t brk_start;
    uint64_t brk_end;
    uint64_t mmap_base;
    struct process_file_mapping mappings[PROCESS_MAX_FILE_MAPPINGS];
};

/* One descriptor table, shared by every thread of a thread group (fork gives
   the child its own deep copy). refs counts the processes pointing at it. */
struct file_table {
    int refs;
    struct file *fds[PROCESS_MAX_FDS];
    uint8_t fd_flags[PROCESS_MAX_FDS];
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
    int termination_signal;
    int stop_signal;
    int stop_reported;
    int continued_pending;
    uint32_t umask;
    uint64_t cr3;
    struct process_memory *memory;
    int is_thread;
    uint64_t entry;
    uint64_t user_stack_top;
    uint64_t kernel_stack_base;
    uint64_t kernel_stack_top;
    /*
     * The x87/SSE register file, as FXSAVE lays it out. These registers are
     * per-process just like the general ones, and nothing else saves them: the
     * kernel is built with -mgeneral-regs-only so it never touches them, and
     * they are swapped here on every context switch. 16-byte alignment is an
     * FXSAVE requirement, not a preference.
     */
    uint8_t fpu_state[512] __attribute__((aligned(16)));
    uint64_t brk_start;
    uint64_t brk_end;
    uint64_t mmap_base;
    uint64_t fs_base;
    uint64_t clear_child_tid_user;
    uint64_t robust_list_head;
    uint64_t robust_list_length;
    uint64_t signal_stack_pointer;
    uint64_t signal_stack_size;
    int signal_stack_flags;
    int pdeath_signal;
    int dumpable;
    int no_new_privs;
    int child_subreaper;
    int thp_disable;
    uint64_t timerslack_ns;
    uint64_t start_time_ns;
    uint64_t runtime_ns;
    uint64_t last_scheduled_ns;
    uint32_t time_slice_ticks;
    uint64_t involuntary_switches;
    char cmdline[512];
    uint64_t cmdline_length;
    struct vfs_node *cwd;
    struct pty_pair *controlling_pty;
    struct syscall_frame saved_frame;
    /* The descriptor table is a separate refcounted object because threads
       share it (CLONE_FILES): an fd opened by any thread of a group must be
       visible to every other thread immediately, and a per-thread copy taken
       at clone time is not that. GLib is where the difference stops being
       theoretical -- its worker thread read()s an inotify fd the main thread
       opened after the worker started, and with copied tables that read
       returns EBADF, which GLib treats as fatal. fork still deep-copies. */
    struct file_table *files;

    /* Set only while blocked inside wait4(). PROCESS_BLOCKED on its own does
       not mean "waiting for a child" -- a rewound read/write sleeping on a wait
       channel is blocked too -- and the wakeup path writes the reaped pid into
       saved_frame.rax, so it must never fire on a non-wait4 sleeper. */
    int wait4_active;
    int64_t wait_pid;
    uint64_t wait_status_user;
    int wait_options;

    int io_wait_active;
    uint64_t io_wait_syscall;
    uint64_t io_wait_deadline_ns;

    /* Set while the saved frame has been rewound to re-issue a blocking
       syscall; lets signal delivery turn the retry into -EINTR. */
    int syscall_rewound;

    int futex_wait_active;
    uint64_t futex_wait_address;
    /* Non-NULL while blocked in process_sleep_on(). */
    const void *wait_channel;
    uint64_t futex_wait_deadline_ns;

    uint64_t itimer_real_interval_ns;
    uint64_t itimer_real_deadline_ns;

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
void process_timer_interrupt(struct interrupt_frame *frame);
/* Map another user stack page for a fault inside the stack growth window.
   Returns 1 when the faulting instruction should simply be retried. */
/*
 * The shared-file-mapping table behind mremap(). Addresses and lengths must be
 * page aligned; the record takes its own reference on `file`.
 */
int process_record_file_mapping(uint64_t start, uint64_t length,
                                struct file *file, uint64_t offset);
/* Drops any record overlapping [start, end), releasing its file reference.
   Called whenever pages are unmapped, so the table cannot outlive the pages. */
void process_forget_file_mappings(uint64_t start, uint64_t end);
/* The record covering `address`, or NULL when nothing shared-file-backed is
   mapped there (an anonymous mapping, for instance). */
struct process_file_mapping *process_find_file_mapping(uint64_t address);

int process_grow_user_stack(uint64_t fault_address);
int process_handle_cow_fault(uint64_t fault_address);
/* Deliver a user-mode CPU exception as a signal. Returns 0 when the fault came
   from kernel mode, which the caller must treat as fatal. */
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

/*
 * Sleep on an opaque channel -- any stable kernel address identifying what is
 * being waited for. Returns 0 once the caller has been woken, or -EAGAIN when
 * nothing else was runnable and switching away would have hung the machine; in
 * that case the caller must fall back to retrying.
 *
 * Callers rewind the syscall so it re-executes on wake, which means a wakeup is
 * only a hint: the condition is always re-tested, so a spurious wake is safe.
 */
int process_sleep_on(struct syscall_frame *frame, const void *channel);
int process_wake_all(const void *channel);
uint32_t process_get_umask(void);
uint32_t process_set_umask(uint32_t mask);
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
