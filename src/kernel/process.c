#include <stddef.h>
#include <stdint.h>
#include "include/build_config.h"
#include "include/elf.h"
#include "include/file.h"
#include "include/gdt.h"
#include "include/heap.h"
#include "include/kstring.h"
#include "include/process.h"
#include "include/procfs.h"
#include "include/syscall.h"
#include "include/time.h"
#include "include/tty.h"
#include "include/vfs.h"
#include "include/vmm.h"

#define KERNEL_STACK_SIZE (16 * 1024)
#define ECHILD 10
#define EINTR 4
#define EINVAL 22
#define ESRCH 3
#define EPERM 1
#define EAGAIN 11
#define EFAULT 14
#define ETIMEDOUT 110
#define SIGSEGV 11
#define IA32_FS_BASE 0xC0000100U

extern void process_enter_user(uint64_t entry, uint64_t user_stack, uint64_t cr3) __attribute__((noreturn));
extern void kprintf(const char *fmt, ...);
extern void panic(const char *msg) __attribute__((noreturn));

#if TUNIX_DEBUG_LOGS
#define KDEBUG(...) kprintf(__VA_ARGS__)
#else
#define KDEBUG(...) do { } while (0)
#endif

static struct process *queue;
static struct process *current;
static uint64_t next_pid = 1;

static struct process_memory *memory_create(uint64_t cr3, uint64_t brk_start,
                                            uint64_t brk_end, uint64_t mmap_base) {
    struct process_memory *memory = (struct process_memory *)kmalloc(sizeof(*memory));
    if (!memory) return NULL;
    memory->cr3 = cr3;
    memory->refs = 1;
    memory->brk_start = brk_start;
    memory->brk_end = brk_end;
    memory->mmap_base = mmap_base;
    return memory;
}

static void memory_ref(struct process_memory *memory) {
    if (memory) memory->refs++;
}

static void memory_unref(struct process_memory *memory) {
    if (!memory || memory->refs == 0) return;
    memory->refs--;
    if (memory->refs != 0) return;
    if (memory->cr3) vmm_destroy_address_space(memory->cr3);
    kfree(memory);
}

static void sync_memory_view(struct process *process) {
    if (!process || !process->memory) return;
    process->cr3 = process->memory->cr3;
    process->brk_start = process->memory->brk_start;
    process->brk_end = process->memory->brk_end;
    process->mmap_base = process->memory->mmap_base;
}

static inline void wrmsr(uint32_t msr, uint64_t value) {
    uint32_t low = (uint32_t)value;
    uint32_t high = (uint32_t)(value >> 32);
    __asm__ volatile("wrmsr" : : "c"(msr), "a"(low), "d"(high));
}


static void set_process_cmdline(struct process *process, const char *path,
                                const char *const argv[]) {
    process->cmdline_length = 0;
    if (argv) {
        for (size_t index = 0; argv[index] && process->cmdline_length + 1 < sizeof(process->cmdline); index++) {
            size_t length = strlen(argv[index]);
            size_t available = sizeof(process->cmdline) - process->cmdline_length - 1;
            if (length > available) length = available;
            memcpy(process->cmdline + process->cmdline_length, argv[index], length);
            process->cmdline_length += length;
            process->cmdline[process->cmdline_length++] = '\0';
        }
    }
    if (!process->cmdline_length && path) {
        size_t length = strlen(path);
        if (length >= sizeof(process->cmdline)) length = sizeof(process->cmdline) - 1;
        memcpy(process->cmdline, path, length);
        process->cmdline[length] = '\0';
        process->cmdline_length = length + 1;
    }
}

static uint64_t signal_bit(int signal_number) {
    if (signal_number < 1 || signal_number > TUNIX_NSIG) return 0;
    return 1ULL << (signal_number - 1);
}

void process_init(void) {
    queue = NULL;
    current = NULL;
}

static void enqueue(struct process *process) {
    if (!queue) {
        queue = process;
        process->next = process;
        return;
    }
    struct process *tail = queue;
    while (tail->next != queue) tail = tail->next;
    tail->next = process;
    process->next = queue;
}

struct process *process_find(uint64_t pid) {
    if (!queue || pid == 0) return NULL;
    struct process *item = queue;
    do {
        if (item->pid == pid && item->state != PROCESS_DEAD) return item;
        item = item->next;
    } while (item != queue);
    return NULL;
}

int process_install_file(struct process *process, struct file *file, int minimum_fd) {
    if (!process || !file) return -1;
    if (minimum_fd < 0) minimum_fd = 0;
    for (int fd = minimum_fd; fd < PROCESS_MAX_FDS; fd++) {
        if (!process->fds[fd]) {
            process->fds[fd] = file;
            return fd;
        }
    }
    return -1;
}

int process_close_fd(struct process *process, int fd) {
    if (!process || fd < 0 || fd >= PROCESS_MAX_FDS || !process->fds[fd]) return -1;
    struct file *file = process->fds[fd];
    process->fds[fd] = NULL;
    file_unref(file);
    return 0;
}

static void close_all_files(struct process *process) {
    for (int fd = 0; fd < PROCESS_MAX_FDS; fd++) {
        if (process->fds[fd]) process_close_fd(process, fd);
    }
}

static void allocate_kernel_stack(struct process *process) {
    uint8_t *kernel_stack = (uint8_t *)kmalloc(KERNEL_STACK_SIZE);
    process->kernel_stack_base = (uint64_t)kernel_stack;
    process->kernel_stack_top = ((uint64_t)kernel_stack + KERNEL_STACK_SIZE) & ~15ULL;
}

static void destroy_process_resources(struct process *process) {
    if (!process) return;
    uint64_t pid = process->pid;
#if !TUNIX_DEBUG_LOGS
    (void)pid;
#endif
    procfs_unregister_process(process->pid);
    if (process->memory) {
        memory_unref(process->memory);
        process->memory = NULL;
        process->cr3 = 0;
    } else if (process->cr3) {
        vmm_destroy_address_space(process->cr3);
        process->cr3 = 0;
    }
    if (process->kernel_stack_base) {
        kfree((void *)process->kernel_stack_base);
        process->kernel_stack_base = 0;
        process->kernel_stack_top = 0;
    }
    kfree(process);
    KDEBUG("process: reaped pid=%u\n", (unsigned)pid);
}

void process_reap_deferred(void) {
    for (;;) {
        if (!queue) return;

        struct process *tail = queue;
        while (tail->next != queue) tail = tail->next;

        struct process *previous = tail;
        struct process *item = queue;
        struct process *victim = NULL;
        do {
            if (item != current && item->state == PROCESS_DEAD) {
                victim = item;
                break;
            }
            previous = item;
            item = item->next;
        } while (item != queue);

        if (!victim) return;
        if (victim->next == victim) {
            queue = NULL;
        } else {
            previous->next = victim->next;
            if (queue == victim) queue = victim->next;
        }
        victim->next = NULL;
        destroy_process_resources(victim);
    }
}

static void install_console(struct process *process) {
    struct vfs_node *console_node = vfs_lookup("/dev/console");
    struct file *console = file_open_node(console_node, 2);
    process->fds[0] = console;
    file_ref(console);
    process->fds[1] = console;
    file_ref(console);
    process->fds[2] = console;
}

struct process *process_create_from_path(const char *path) {
    struct vfs_node *file = vfs_lookup(path);
    if (!file) {
        kprintf("process: executable not found: %s\n", path);
        return NULL;
    }

    struct process *process = (struct process *)kmalloc(sizeof(*process));
    if (!process) {
        kprintf("process: allocation failed for %s\n", path);
        return NULL;
    }
    memset(process, 0, sizeof(*process));
    process->pid = next_pid++;
    process->tgid = process->pid;
    process->ppid = 0;
    process->pgid = process->pid;
    process->sid = process->pid;
    process->state = PROCESS_READY;
    process->cwd = vfs_root;
    strncpy(process->name, file->name, sizeof(process->name) - 1);
    strncpy(process->exe_path, path, sizeof(process->exe_path) - 1);
    process->cr3 = vmm_create_address_space();
    if (!process->cr3) {
        kprintf("process: address-space creation failed for %s\n", path);
        kfree(process);
        return NULL;
    }
    process->start_time_ns = time_uptime_ns();

    const char *argv[] = {path, NULL};
    const char *envp[] = {
        "PATH=/bin:/sbin",
        "HOME=/",
        "TERM=tunix",
        "SHELL=/bin/bash",
        "USER=root",
        NULL
    };
    set_process_cmdline(process, path, argv);
    if (elf_load_process(process, file, argv, envp) != 0) {
        kprintf("process: invalid ELF64: %s\n", path);
        vmm_destroy_address_space(process->cr3);
        kfree(process);
        return NULL;
    }
    process->memory = memory_create(process->cr3, process->brk_start,
                                    process->brk_end, process->mmap_base);
    if (!process->memory) {
        vmm_destroy_address_space(process->cr3);
        kfree(process);
        return NULL;
    }

    allocate_kernel_stack(process);
    process->saved_frame.user_rip = process->entry;
    process->saved_frame.user_rsp = process->user_stack_top;
    process->saved_frame.user_rflags = 0x202;
    install_console(process);

    enqueue(process);
    procfs_register_process(process);
    if (process->pid == 1) tty_set_foreground_pgid((int)process->pgid);
    KDEBUG("process: pid=%u path=%s entry=%p cr3=%p\n",
            (unsigned)process->pid, path, (void *)process->entry, (void *)process->cr3);
    return process;
}

struct process *process_current(void) { return current; }
uint64_t process_current_pid(void) { return current ? current->tgid : 0; }
uint64_t process_current_tid(void) { return current ? current->pid : 0; }
uint64_t process_current_ppid(void) { return current ? current->ppid : 0; }

static int runnable(const struct process *process) {
    return process && (process->state == PROCESS_READY || process->state == PROCESS_RUNNING);
}

static void wake_expired_futex_waiters(void) {
    if (!queue) return;
    uint64_t now = time_uptime_ns();
    struct process *item = queue;
    do {
        if (item->state == PROCESS_BLOCKED && item->futex_wait_active &&
            item->futex_wait_deadline_ns != UINT64_MAX &&
            now >= item->futex_wait_deadline_ns) {
            item->futex_wait_active = 0;
            item->futex_wait_address = 0;
            item->futex_wait_deadline_ns = 0;
            item->saved_frame.rax = (uint64_t)-(int64_t)ETIMEDOUT;
            item->state = PROCESS_READY;
        }
        item = item->next;
    } while (item != queue);
}

static struct process *next_runnable(struct process *after) {
    if (!queue) return NULL;
    wake_expired_futex_waiters();
    struct process *candidate = after ? after->next : queue;
    struct process *start = candidate;
    do {
        if (runnable(candidate)) return candidate;
        candidate = candidate->next;
    } while (candidate != start);
    return NULL;
}

static void activate_process(struct process *process) {
    current = process;
    process->last_scheduled_ns = time_uptime_ns();
    process->state = PROCESS_RUNNING;
    set_kernel_stack(process->kernel_stack_top);
    syscall_set_kernel_stack(process->kernel_stack_top);
    vmm_activate(process->cr3);
    wrmsr(IA32_FS_BASE, process->fs_base);
}

static int switch_to_next(struct syscall_frame *frame, struct process *after) {
    struct process *next = next_runnable(after);
    if (!next) return -1;
    *frame = next->saved_frame;
    activate_process(next);
    return 0;
}

void process_start_first(void) {
    struct process *first = next_runnable(NULL);
    if (!first) panic("process: no runnable userspace program");
    activate_process(first);
    process_enter_user(first->entry, first->user_stack_top, first->cr3);
    panic("process_enter_user returned");
}

void process_yield_from_syscall(struct syscall_frame *frame) {
    if (!current || !frame) return;
    struct process *yielding = current;
    yielding->saved_frame = *frame;
    yielding->state = PROCESS_READY;
    struct process *next = next_runnable(yielding);
    if (!next || next == yielding) {
        yielding->state = PROCESS_RUNNING;
        return;
    }
    *frame = next->saved_frame;
    activate_process(next);
}

void process_run_child_first_from_syscall(struct syscall_frame *frame, uint64_t child_pid) {
    if (!current || !frame || child_pid == 0) return;

    struct process *parent = current;
    struct process *child = process_find(child_pid);
    if (!child || child->state != PROCESS_READY ||
        (child->ppid != parent->pid &&
         !(child->is_thread && child->tgid == parent->tgid))) return;

    parent->saved_frame = *frame;
    parent->state = PROCESS_READY;
    *frame = child->saved_frame;
    activate_process(child);
}

static struct process *find_parent(struct process *child) {
    return child && child->ppid ? process_find(child->ppid) : NULL;
}

static int child_matches(const struct process *child, const struct process *parent, int64_t requested) {
    if (!child || !parent || child->ppid != parent->pid) return 0;
    if (requested > 0) return child->pid == (uint64_t)requested;
    if (requested == -1) return 1;
    if (requested == 0) return child->pgid == parent->pgid;
    return child->pgid == (uint64_t)(-requested);
}

static int store_wait_status(struct process *parent, struct process *child, uint64_t status_user) {
    if (!status_user) return 0;
    int status = (child->exit_status & 0xFF) << 8;
    return vmm_copy_to_space(parent->cr3, status_user, &status, sizeof(status));
}

static int store_job_status(struct process *parent, int status, uint64_t status_user) {
    if (!status_user) return 0;
    return vmm_copy_to_space(parent->cr3, status_user, &status, sizeof(status));
}

static void notify_parent_of_exit(struct process *child) {
    struct process *parent = find_parent(child);
    if (!parent) {
        child->state = PROCESS_DEAD;
        return;
    }

    parent->signal_pending |= signal_bit(SIGCHLD);
    if (parent->state == PROCESS_BLOCKED && child_matches(child, parent, parent->wait_pid)) {
        if (store_wait_status(parent, child, parent->wait_status_user) == 0) parent->saved_frame.rax = child->pid;
        else parent->saved_frame.rax = (uint64_t)-(int64_t)EINVAL;
        parent->wait_pid = 0;
        parent->wait_status_user = 0;
        parent->wait_options = 0;
        parent->state = PROCESS_READY;
        child->state = PROCESS_DEAD;
    }
}

static int notify_parent_of_job_change(struct process *child, int wait_flag, int status) {
    struct process *parent = find_parent(child);
    if (!parent) return 0;

    parent->signal_pending |= signal_bit(SIGCHLD);
    if (parent->state != PROCESS_BLOCKED ||
        !child_matches(child, parent, parent->wait_pid) ||
        !(parent->wait_options & wait_flag)) return 0;

    if (store_job_status(parent, status, parent->wait_status_user) == 0)
        parent->saved_frame.rax = child->pid;
    else
        parent->saved_frame.rax = (uint64_t)-(int64_t)EINVAL;
    parent->wait_pid = 0;
    parent->wait_status_user = 0;
    parent->wait_options = 0;
    parent->state = PROCESS_READY;
    return 1;
}

void process_exit_from_syscall(struct syscall_frame *frame, int status) {
    if (!current || !frame) panic("process: exit without current process");
    struct process *exiting = current;
    exiting->exit_status = status;
    exiting->state = PROCESS_ZOMBIE;
    if (exiting->clear_child_tid_user) {
        uint64_t clear_address = exiting->clear_child_tid_user;
        uint32_t zero = 0;
        (void)vmm_copy_to_space(exiting->cr3, clear_address, &zero, sizeof(zero));
        exiting->clear_child_tid_user = 0;
        (void)process_futex_wake(clear_address, 1);
    }
    close_all_files(exiting);
    if (exiting->is_thread) exiting->state = PROCESS_DEAD;
    else notify_parent_of_exit(exiting);
    KDEBUG("process: pid=%u exited status=%d\n", (unsigned)exiting->pid, status);

    if (switch_to_next(frame, exiting) != 0) {
        KDEBUG("userspace halted: no runnable processes\n");
        for (;;) __asm__ volatile("cli; hlt");
    }
}

int64_t process_fork_from_syscall(struct syscall_frame *frame) {
    if (!current || !frame) return -EINVAL;
    struct process *parent = current;
    struct process *child = (struct process *)kmalloc(sizeof(*child));
    memset(child, 0, sizeof(*child));

    child->pid = next_pid++;
    child->tgid = child->pid;
    child->ppid = parent->pid;
    child->pgid = parent->pgid;
    child->sid = parent->sid;
    child->state = PROCESS_READY;
    child->cwd = parent->cwd;
    child->controlling_pty = parent->controlling_pty;
    strncpy(child->name, parent->name, sizeof(child->name) - 1);
    strncpy(child->exe_path, parent->exe_path, sizeof(child->exe_path) - 1);
    child->cr3 = vmm_clone_address_space(parent->cr3);
    if (!child->cr3) {
        kfree(child);
        return -EINVAL;
    }
    uint64_t parent_brk_start = parent->memory ? parent->memory->brk_start : parent->brk_start;
    uint64_t parent_brk_end = parent->memory ? parent->memory->brk_end : parent->brk_end;
    uint64_t parent_mmap_base = parent->memory ? parent->memory->mmap_base : parent->mmap_base;
    child->memory = memory_create(child->cr3, parent_brk_start, parent_brk_end,
                                  parent_mmap_base);
    if (!child->memory) {
        vmm_destroy_address_space(child->cr3);
        kfree(child);
        return -EINVAL;
    }
    child->entry = parent->entry;
    child->user_stack_top = parent->user_stack_top;
    child->brk_start = parent_brk_start;
    child->brk_end = parent_brk_end;
    child->mmap_base = parent_mmap_base;
    child->fs_base = parent->fs_base;
    child->start_time_ns = time_uptime_ns();
    child->runtime_ns = 0;
    child->last_scheduled_ns = 0;
    child->cmdline_length = parent->cmdline_length;
    memcpy(child->cmdline, parent->cmdline, sizeof(child->cmdline));
    allocate_kernel_stack(child);

    child->saved_frame = *frame;
    child->saved_frame.rax = 0;
    child->signal_blocked = parent->signal_blocked;
    memcpy(child->signal_actions, parent->signal_actions, sizeof(child->signal_actions));

    for (int fd = 0; fd < PROCESS_MAX_FDS; fd++) {
        if (parent->fds[fd]) {
            child->fds[fd] = parent->fds[fd];
            file_ref(child->fds[fd]);
        }
    }

    enqueue(child);
    procfs_register_process(child);
    KDEBUG("process: fork parent=%u child=%u\n", (unsigned)parent->pid, (unsigned)child->pid);
    return (int64_t)child->pid;
}


int64_t process_clone_thread_from_syscall(struct syscall_frame *frame,
                                          uint64_t child_stack, uint64_t tls,
                                          uint64_t parent_tid_user,
                                          uint64_t child_tid_user,
                                          uint64_t flags) {
    if (!current || !frame || !child_stack || child_stack >= USER_ADDRESS_LIMIT)
        return -EINVAL;
    if (!current->memory) return -EINVAL;

    struct process *parent = current;
    struct process *child = (struct process *)kmalloc(sizeof(*child));
    if (!child) return -EINVAL;
    memset(child, 0, sizeof(*child));

    child->pid = next_pid++;
    child->tgid = parent->tgid;
    child->ppid = parent->ppid;
    child->pgid = parent->pgid;
    child->sid = parent->sid;
    child->state = PROCESS_READY;
    child->is_thread = 1;
    child->cwd = parent->cwd;
    child->controlling_pty = parent->controlling_pty;
    strncpy(child->name, parent->name, sizeof(child->name) - 1);
    strncpy(child->exe_path, parent->exe_path, sizeof(child->exe_path) - 1);
    child->memory = parent->memory;
    memory_ref(child->memory);
    sync_memory_view(child);
    child->entry = parent->entry;
    child->user_stack_top = child_stack;
    child->fs_base = (flags & 0x00080000ULL) ? tls : parent->fs_base;
    child->start_time_ns = time_uptime_ns();
    child->cmdline_length = parent->cmdline_length;
    memcpy(child->cmdline, parent->cmdline, sizeof(child->cmdline));
    allocate_kernel_stack(child);

    child->saved_frame = *frame;
    child->saved_frame.rax = 0;
    child->saved_frame.user_rsp = child_stack;
    child->signal_blocked = parent->signal_blocked;
    memcpy(child->signal_actions, parent->signal_actions, sizeof(child->signal_actions));

    for (int fd = 0; fd < PROCESS_MAX_FDS; fd++) {
        if (parent->fds[fd]) {
            child->fds[fd] = parent->fds[fd];
            file_ref(child->fds[fd]);
        }
    }

    uint32_t tid = (uint32_t)child->pid;
    if ((flags & 0x00100000ULL) && parent_tid_user &&
        vmm_copy_to_space(parent->cr3, parent_tid_user, &tid, sizeof(tid)) != 0) {
        close_all_files(child);
        memory_unref(child->memory);
        kfree((void *)child->kernel_stack_base);
        kfree(child);
        return -EFAULT;
    }
    if ((flags & (0x01000000ULL | 0x00200000ULL)) && child_tid_user) {
        if ((flags & 0x01000000ULL) &&
            vmm_copy_to_space(child->cr3, child_tid_user, &tid, sizeof(tid)) != 0) {
            close_all_files(child);
            memory_unref(child->memory);
            kfree((void *)child->kernel_stack_base);
            kfree(child);
            return -EFAULT;
        }
        if (flags & 0x00200000ULL) child->clear_child_tid_user = child_tid_user;
    }

    enqueue(child);
    procfs_register_process(child);
    KDEBUG("process: clone thread tgid=%u tid=%u\n",
           (unsigned)child->tgid, (unsigned)child->pid);
    return (int64_t)child->pid;
}

int64_t process_futex_wait(struct syscall_frame *frame, uint64_t address,
                           uint32_t expected, int64_t timeout_ns) {
    if (!current || !frame || (address & 3U) || address >= USER_ADDRESS_LIMIT)
        return -EINVAL;
    uint32_t value = 0;
    if (vmm_copy_from_space(current->cr3, &value, address, sizeof(value)) != 0)
        return -EFAULT;
    if (value != expected) return -EAGAIN;
    if (timeout_ns == 0) return -ETIMEDOUT;

    struct process *waiting = current;
    waiting->saved_frame = *frame;
    waiting->saved_frame.rax = 0;
    waiting->state = PROCESS_BLOCKED;
    waiting->futex_wait_active = 1;
    waiting->futex_wait_address = address;
    waiting->futex_wait_deadline_ns = timeout_ns < 0 ? UINT64_MAX :
        time_uptime_ns() + (uint64_t)timeout_ns;
    if (switch_to_next(frame, waiting) != 0) {
        waiting->state = PROCESS_RUNNING;
        waiting->futex_wait_active = 0;
        waiting->futex_wait_address = 0;
        waiting->futex_wait_deadline_ns = 0;
        return -EAGAIN;
    }
    return 0;
}

int process_futex_wake(uint64_t address, int maximum) {
    if (!current || !queue || maximum <= 0) return 0;
    int woken = 0;
    struct process *item = queue;
    do {
        if (item->state == PROCESS_BLOCKED && item->futex_wait_active &&
            item->memory == current->memory &&
            item->futex_wait_address == address) {
            item->futex_wait_active = 0;
            item->futex_wait_address = 0;
            item->futex_wait_deadline_ns = 0;
            item->saved_frame.rax = 0;
            item->state = PROCESS_READY;
            woken++;
            if (woken >= maximum) break;
        }
        item = item->next;
    } while (item != queue);
    return woken;
}

void process_exit_group_from_syscall(struct syscall_frame *frame, int status) {
    if (!current || !frame) panic("process: exit_group without current process");
    uint64_t group = current->tgid;
    if (queue) {
        struct process *item = queue;
        do {
            if (item != current && item->tgid == group && item->state != PROCESS_DEAD) {
                item->exit_status = status;
                if (item->clear_child_tid_user) {
                    uint64_t clear_address = item->clear_child_tid_user;
                    uint32_t zero = 0;
                    (void)vmm_copy_to_space(item->cr3, clear_address, &zero, sizeof(zero));
                    item->clear_child_tid_user = 0;
                    (void)process_futex_wake(clear_address, 1);
                }
                item->state = PROCESS_DEAD;
                close_all_files(item);
            }
            item = item->next;
        } while (item != queue);
    }
    current->is_thread = 0;
    process_exit_from_syscall(frame, status);
}

int64_t process_exec_from_syscall(struct syscall_frame *frame, const char *path,
                                  const char *const argv[], const char *const envp[]) {
    if (!current || !frame || !path) return -EINVAL;
    struct vfs_node *file = vfs_lookup(path);
    if (!file) return -1;

    uint64_t new_cr3 = vmm_create_address_space();
    struct process image;
    memset(&image, 0, sizeof(image));
    image.cr3 = new_cr3;
    if (elf_load_process(&image, file, argv, envp) != 0) {
        vmm_destroy_address_space(new_cr3);
        return -1;
    }

    struct process_memory *new_memory = memory_create(new_cr3, image.brk_start,
                                                       image.brk_end, image.mmap_base);
    if (!new_memory) {
        vmm_destroy_address_space(new_cr3);
        return -EINVAL;
    }
    struct process_memory *old_memory = current->memory;
    uint64_t old_cr3 = current->cr3;
    current->memory = new_memory;
    current->cr3 = new_cr3;
    current->tgid = current->pid;
    current->is_thread = 0;
    current->entry = image.entry;
    current->user_stack_top = image.user_stack_top;
    current->brk_start = image.brk_start;
    current->brk_end = image.brk_end;
    current->mmap_base = image.mmap_base;
    current->fs_base = 0;
    strncpy(current->name, file->name, sizeof(current->name) - 1);
    strncpy(current->exe_path, path, sizeof(current->exe_path) - 1);
    set_process_cmdline(current, path, argv);
    for (int sig = 0; sig < TUNIX_NSIG; sig++) {
        if (current->signal_actions[sig].handler != SIG_IGN) memset(&current->signal_actions[sig], 0, sizeof(current->signal_actions[sig]));
    }
    current->signal_pending = 0;
    current->in_signal = 0;

    memset(frame, 0, sizeof(*frame));
    frame->user_rip = current->entry;
    frame->user_rsp = current->user_stack_top;
    frame->user_rflags = 0x202;
    vmm_activate(new_cr3);
    wrmsr(IA32_FS_BASE, 0);
    if (old_memory) memory_unref(old_memory);
    else vmm_destroy_address_space(old_cr3);
    KDEBUG("process: pid=%u exec %s\n", (unsigned)current->pid, path);
    return 0;
}

int64_t process_waitpid_from_syscall(struct syscall_frame *frame, int64_t pid,
                                     uint64_t status_user, int options) {
    if (!current || !frame || (options & ~(WNOHANG | WUNTRACED | WCONTINUED))) return -EINVAL;
    struct process *parent = current;
    int has_child = 0;
    struct process *item = queue;
    if (item) {
        do {
            if (child_matches(item, parent, pid)) {
                has_child = 1;
                if (item->state == PROCESS_ZOMBIE) {
                    if (store_wait_status(parent, item, status_user) != 0) return -EINVAL;
                    item->state = PROCESS_DEAD;
                    return (int64_t)item->pid;
                }
                if ((options & WUNTRACED) && item->state == PROCESS_STOPPED &&
                    !item->stop_reported) {
                    int status = ((item->stop_signal & 0xFF) << 8) | 0x7F;
                    if (store_job_status(parent, status, status_user) != 0) return -EINVAL;
                    item->stop_reported = 1;
                    return (int64_t)item->pid;
                }
                if ((options & WCONTINUED) && item->continued_pending) {
                    if (store_job_status(parent, 0xFFFF, status_user) != 0) return -EINVAL;
                    item->continued_pending = 0;
                    return (int64_t)item->pid;
                }
            }
            item = item->next;
        } while (item != queue);
    }
    if (!has_child) return -ECHILD;
    if (options & WNOHANG) return 0;

    parent->saved_frame = *frame;
    parent->state = PROCESS_BLOCKED;
    parent->wait_pid = pid;
    parent->wait_status_user = status_user;
    parent->wait_options = options;
    if (switch_to_next(frame, parent) != 0) {
        parent->state = PROCESS_RUNNING;
        parent->wait_pid = 0;
        parent->wait_status_user = 0;
        parent->wait_options = 0;
        return -ECHILD;
    }
    return 0;
}

static void signal_one_process(struct process *target, int signal_number) {
    if (signal_number == 0) return;
    if (signal_number == SIGKILL && target->state == PROCESS_STOPPED)
        target->state = PROCESS_READY;
    if (signal_number == SIGCONT && target->state == PROCESS_STOPPED) {
        target->state = PROCESS_READY;
        target->continued_pending = 1;
        target->stop_reported = 0;
        target->signal_pending &= ~(signal_bit(SIGSTOP) | signal_bit(SIGTSTP));
        if (notify_parent_of_job_change(target, WCONTINUED, 0xFFFF))
            target->continued_pending = 0;
    }
    target->signal_pending |= signal_bit(signal_number);
    if (target->state == PROCESS_BLOCKED && signal_number != SIGCHLD) {
        target->futex_wait_active = 0;
        target->futex_wait_address = 0;
        target->futex_wait_deadline_ns = 0;
        target->saved_frame.rax = (uint64_t)-(int64_t)EINTR;
        target->wait_pid = 0;
        target->wait_status_user = 0;
        target->wait_options = 0;
        target->state = PROCESS_READY;
    }
}

int process_send_signal(int64_t pid, int signal_number) {
    if (signal_number < 0 || signal_number > TUNIX_NSIG || !current) return -EINVAL;
    if (pid > 0) {
        struct process *target = process_find((uint64_t)pid);
        if (!target) return -ESRCH;
        signal_one_process(target, signal_number);
        return 0;
    }

    uint64_t group = pid == 0 ? current->pgid : (uint64_t)(-pid);
    int delivered = 0;
    if (!queue) return -ESRCH;
    struct process *target = queue;
    do {
        int match = pid == -1 ? target->pid != 1 : target->pgid == group;
        if (match && target->state != PROCESS_DEAD) {
            signal_one_process(target, signal_number);
            delivered = 1;
        }
        target = target->next;
    } while (target != queue);
    return delivered ? 0 : -ESRCH;
}

int process_setpgid(int64_t pid, int64_t pgid) {
    if (!current) return -EINVAL;
    struct process *target = pid == 0 ? current : process_find((uint64_t)pid);
    if (!target) return -ESRCH;
    if (target != current && target->ppid != current->pid) return -EPERM;
    if (pgid == 0) pgid = (int64_t)target->pid;
    if (pgid < 0) return -EINVAL;
    target->pgid = (uint64_t)pgid;
    return 0;
}

int64_t process_setsid(void) {
    if (!current) return -EINVAL;
    if (current->pgid == current->pid) return -EPERM;
    current->sid = current->pid;
    current->pgid = current->pid;
    current->controlling_pty = NULL;
    return (int64_t)current->sid;
}

int process_sigreturn(struct syscall_frame *frame) {
    if (!current || !frame || !current->in_signal) return -EINVAL;
    *frame = current->signal_saved_frame;
    current->signal_blocked = current->signal_saved_mask;
    current->in_signal = 0;
    return 0;
}

static int next_pending_signal(struct process *process) {
    uint64_t available = process->signal_pending & ~process->signal_blocked;
    available |= process->signal_pending & signal_bit(SIGKILL);
    if (!available) return 0;
    for (int signal_number = 1; signal_number <= TUNIX_NSIG; signal_number++) {
        if (available & signal_bit(signal_number)) return signal_number;
    }
    return 0;
}

void process_prepare_user_return(struct syscall_frame *frame) {
    if (!current || !frame || current->state != PROCESS_RUNNING || current->in_signal) return;
    int signal_number = next_pending_signal(current);
    if (!signal_number) return;
    uint64_t bit = signal_bit(signal_number);
    current->signal_pending &= ~bit;
    struct tunix_sigaction *action = &current->signal_actions[signal_number - 1];

    if (signal_number != SIGKILL && signal_number != SIGSTOP &&
        (action->handler == SIG_IGN ||
         (action->handler == SIG_DFL &&
          (signal_number == SIGCHLD || signal_number == SIGCONT)))) return;
    if (signal_number == SIGSTOP ||
        (action->handler == SIG_DFL && signal_number == SIGTSTP)) {
        current->stop_signal = signal_number;
        current->stop_reported = 0;
        current->continued_pending = 0;
        current->state = PROCESS_STOPPED;
        current->stop_reported = notify_parent_of_job_change(
            current, WUNTRACED, ((signal_number & 0xFF) << 8) | 0x7F);
        if (switch_to_next(frame, current) != 0) {
            current->state = PROCESS_RUNNING;
        }
        return;
    }
    if (action->handler == SIG_DFL || signal_number == SIGKILL) {
        process_exit_from_syscall(frame, 128 + signal_number);
        return;
    }
    if (action->handler >= USER_ADDRESS_LIMIT || action->restorer >= USER_ADDRESS_LIMIT) {
        process_exit_from_syscall(frame, 128 + SIGSEGV);
        return;
    }

    uint64_t new_rsp = (frame->user_rsp & ~15ULL) - 8;
    if (vmm_copy_to_space(current->cr3, new_rsp, &action->restorer, sizeof(action->restorer)) != 0) {
        process_exit_from_syscall(frame, 128 + SIGSEGV);
        return;
    }
    current->signal_saved_frame = *frame;
    current->signal_saved_mask = current->signal_blocked;
    current->signal_blocked |= action->mask | bit;
    current->in_signal = 1;
    frame->user_rsp = new_rsp;
    frame->user_rip = action->handler;
    frame->rdi = (uint64_t)signal_number;
    frame->rax = 0;
}

void process_set_fs_base(uint64_t value) {
    if (!current || value >= USER_ADDRESS_LIMIT) return;
    current->fs_base = value;
    wrmsr(IA32_FS_BASE, value);
}

uint64_t process_get_fs_base(void) {
    return current ? current->fs_base : 0;
}

void process_account_runtime(void) {
    if (!current || current->state != PROCESS_RUNNING || !current->last_scheduled_ns) return;
    uint64_t now = time_uptime_ns();
    if (now >= current->last_scheduled_ns) current->runtime_ns += now - current->last_scheduled_ns;
    current->last_scheduled_ns = now;
}

uint64_t process_runtime_ns(const struct process *process) {
    if (!process) return 0;
    uint64_t runtime = process->runtime_ns;
    if (process == current && process->state == PROCESS_RUNNING && process->last_scheduled_ns) {
        uint64_t now = time_uptime_ns();
        if (now >= process->last_scheduled_ns) runtime += now - process->last_scheduled_ns;
    }
    return runtime;
}

uint64_t process_count(void) {
    if (!queue) return 0;
    uint64_t count = 0;
    struct process *item = queue;
    do {
        if (item->state != PROCESS_DEAD) count++;
        item = item->next;
    } while (item != queue);
    return count;
}

uint64_t process_created_count(void) {
    return next_pid - 1;
}

uint64_t process_runnable_count(void) {
    if (!queue) return 0;
    uint64_t count = 0;
    struct process *item = queue;
    do {
        if (item->state == PROCESS_READY || item->state == PROCESS_RUNNING) count++;
        item = item->next;
    } while (item != queue);
    return count;
}

uint64_t process_blocked_count(void) {
    if (!queue) return 0;
    uint64_t count = 0;
    struct process *item = queue;
    do {
        if (item->state == PROCESS_BLOCKED) count++;
        item = item->next;
    } while (item != queue);
    return count;
}

uint64_t process_total_runtime_ns(void) {
    if (!queue) return 0;
    uint64_t total = 0;
    struct process *item = queue;
    do {
        if (item->state != PROCESS_DEAD) total += process_runtime_ns(item);
        item = item->next;
    } while (item != queue);
    return total;
}
