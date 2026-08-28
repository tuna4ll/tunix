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
/* waitid(2) only. WSTOPPED shares WUNTRACED's value, as on Linux. */
#define WSTOPPED 2
#define WEXITED 4
#define WNOWAIT 0x01000000
/* Thread-selection flags. Tunix has one kind of child, so they select the same
   set either way -- but a caller that passes them (sudo does) must not be told
   its arguments are invalid. */
#define WNOTHREAD 0x20000000
#define WALLCHILDREN 0x40000000
#define WCLONE 0x80000000

struct vfs_node;
struct pty_pair;
struct interrupt_frame;

/*
 * One mapped range of the address space.
 *
 * The kernel used to infer this from the page tables, which can only say what
 * is resident, not what a range is *for*. That is why anonymous memory had to
 * be committed at mmap time -- an untouched page and an unmapped one looked
 * identical -- and why finding a free range meant walking every page of every
 * candidate. With the ranges written down, memory is handed out as address
 * space and paid for a page at a time when it is touched.
 *
 * The list is sorted by start address and never overlaps; splitting is what
 * munmap and mprotect do to it.
 */
#define VM_ANONYMOUS 0x1U
/* The range maps the file's cached contents rather than a copy of them, so the
   area holds one vfs_map_ref() on the node for as long as it exists. Splitting
   an area or inheriting it across fork produces another area and another
   reference, which is what keeps the arithmetic right without a list of
   mappings per file. */
#define VM_FILE_PAGES 0x2U

struct vm_area {
    uint64_t start;
    uint64_t end;
    uint64_t page_flags;  /* what a page committed here is mapped with */
    uint32_t kind;
    /* What backs the range, for the mappings that have a backing object.
       The reference is what keeps it alive once the process closes the
       descriptor but keeps the mapping, and it is what mremap() needs in
       order to extend a mapping over more of that object. */
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
    struct credentials cred;
    uint64_t cr3;
    struct process_memory *memory;
    int is_thread;
    /* Set by another thread of the group that has exited, when this one was
       running on a different processor and so could not be torn down from
       there. It leaves on its own next return to user mode. */
    int group_exit_pending;
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
    /* PR_SET_KEEPCAPS. Recorded and never consulted: there are no
       capabilities to keep across a setuid. */
    int keep_capabilities;
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
    /* What the waiter was told the word held. A dump that shows this next to
       what the word holds now is how a lost wakeup tells itself apart from a
       thread that simply has nothing to wait for yet. */
    uint32_t futex_wait_expected;
    /* Which bits of a FUTEX_WAKE_BITSET this waiter answers to. FUTEX_WAIT
       asks for all of them, which is what makes the two commands one queue. */
    uint32_t futex_wait_bitset;
    /* Non-NULL while blocked in process_sleep_on(). */
    const void *wait_channel;
    uint64_t futex_wait_deadline_ns;

    uint64_t itimer_real_interval_ns;
    uint64_t itimer_real_deadline_ns;

    uint64_t signal_pending;
    /* Which pending signals came from a kill(2) rather than from the kernel;
       it is the difference between SI_USER and SI_KERNEL in siginfo. */
    uint64_t signal_user_sent;
    /* Who sent each of those, for siginfo's si_pid and si_uid. One entry per
       signal is the whole story here: pending signals are a bitmask, not a
       queue, so a second kill(2) of the same signal replaces the first. */
    uint32_t signal_sender_pid[TUNIX_NSIG];
    uint32_t signal_sender_uid[TUNIX_NSIG];
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
/* Print every process, its state, what it is blocked on and where it last was.
   Bound to Ctrl+Alt+D on the console; see the definition for why. */
void process_dump_all(void);
struct process *process_find(uint64_t pid);
uint64_t process_current_pid(void);
uint64_t process_current_tid(void);
uint64_t process_current_ppid(void);
void process_start_first(void) __attribute__((noreturn));
/* Every processor but the first: park until the scheduler has work for it. */
void process_run_idle(void) __attribute__((noreturn));
void process_yield_from_syscall(struct syscall_frame *frame);
void process_timer_interrupt(struct interrupt_frame *frame);
/* Map another user stack page for a fault inside the stack growth window.
   Returns 1 when the faulting instruction should simply be retried. */
/* The address-space map. process_commit_area() answers a not-present fault the
   same way the stack window does: 1 means retry the instruction. */
int process_map_area(uint64_t start, uint64_t end, uint64_t page_flags,
                     uint32_t kind, struct file *file, uint64_t offset);
void process_unmap_area(uint64_t start, uint64_t end);
void process_protect_area(uint64_t start, uint64_t end, uint64_t page_flags);
int process_area_range_free(uint64_t start, uint64_t end);
int process_find_free_range(uint64_t start, uint64_t length, uint64_t *base_out);
int process_commit_area(uint64_t fault_address);
/* The area covering `address`, or NULL when nothing is mapped there. */
struct vm_area *process_find_area(uint64_t address);

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
/* `credential_source` is the node whose set-user-ID bits the new image runs
   with, or NULL when there is no transition (scripts never get one). */
int64_t process_exec_from_syscall(struct syscall_frame *frame, const char *path,
                                  const char *const argv[], const char *const envp[],
                                  const struct vfs_node *credential_source);
int64_t process_waitpid_from_syscall(struct syscall_frame *frame, int64_t pid,
                                     uint64_t status_user, int options);
int64_t process_waitid_from_syscall(int64_t pid_spec, uint64_t info_user,
                                    int options);
int process_send_signal(int64_t pid, int signal_number);
/* kill(2): refuses targets the caller's identity has no business signalling. */
int process_send_signal_checked(int64_t pid, int signal_number);
/* Installs a handler for the whole thread group: CLONE_SIGHAND is mandatory
   for threads, so they share the table even though each holds its own copy. */
void process_set_sigaction(int signal_number, const struct tunix_sigaction *action);
int process_setpgid(int64_t pid, int64_t pgid);
int64_t process_setsid(void);
void process_prepare_user_return(struct syscall_frame *frame);
int process_sigreturn(struct syscall_frame *frame);
/* A wake that reaches every waiter, whichever bits it asked for. */
#define FUTEX_BITSET_MATCH_ANY 0xFFFFFFFFU

int64_t process_futex_wait(struct syscall_frame *frame, uint64_t address,
                           uint32_t expected, int64_t timeout_ns,
                           uint32_t bitset);
int process_futex_wake(uint64_t address, int maximum, uint32_t bitset);

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
/*
 * The channel a syscall waits on when it has nothing better.
 *
 * poll() and select() wait on several descriptors at once, so no single
 * object's wait queue is the right one to sleep on, and some descriptors --
 * a TCP socket, say -- have no queue at all because nothing signals them.
 * Both used to be answered by rewinding the syscall and yielding, which is
 * a spin: an idle machine with a shell at a prompt on each terminal burned
 * three of its four processors doing nothing.
 *
 * So they sleep here instead, and this is woken generously: by any other
 * wakeup (process_wake_all), and by the tick, which is what covers a source
 * that signals nothing at all. Every sleeper re-tests its own condition and
 * its own deadline on waking, so waking too often is only ever wasted work
 * -- and waking too rarely, which a per-source queue would risk, is a hang.
 */
const void *process_io_wait_channel(void);
int process_wake_io(void);
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
