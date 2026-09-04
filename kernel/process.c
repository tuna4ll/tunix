#include <stddef.h>
#include <stdint.h>
#include "include/build_config.h"
#include "include/cred.h"
#include "include/elf.h"
#include "include/file.h"
#include "include/gdt.h"
#include "include/heap.h"
#include "include/interrupt.h"
#include "include/klock.h"
#include "include/oplock.h"

static int process_wake_all_locked(const void *channel);
#include "include/kstring.h"
#include "include/percpu.h"
#include "include/pmm.h"
#include "include/process.h"
#include "include/procfs.h"
#include "include/smp.h"
#include "include/syscall.h"
#include "include/time.h"
#include "include/timer.h"
#include "include/tty.h"
#include "include/vt.h"
#include "include/vfs.h"
#include "include/vmm.h"

/*
 * The kernel stack a process runs its syscalls and interrupts on.
 *
 * 32 KiB rather than 16, for headroom rather than for a bug: measuring the
 * frames says the deepest ordinary path through syscall_dispatch is about
 * 9700 bytes, but sys_read's 4 KiB copy buffer calling a /proc reader with
 * another 4 KiB of its own is most of 16 before the VFS frames between them,
 * and an interrupt lands on top of whatever is there.
 *
 * Running out is not a soft failure. The page below the stack is not mapped
 * once the heap has handed its pages back, so the overflow faults, and the
 * fault handler has no stack to be delivered on either: what the machine
 * actually does is reset, with nothing printed. The double-fault handler is
 * what turns that into a message.
 */
#define KERNEL_STACK_SIZE (32 * 1024)
#define ECHILD 10
#define EINTR 4
#define EINVAL 22
#define ESRCH 3
#define EPERM 1
#define EACCES 13
#define EAGAIN 11
#define EFAULT 14
#define ETIMEDOUT 110
#define SIGSEGV 11
#define IA32_FS_BASE 0xC0000100U
#define FUTEX_OWNER_DIED 0x40000000U
#define FUTEX_TID_MASK 0x3fffffffU
#define ROBUST_LIST_LIMIT 2048U
#define DEFAULT_TIMERSLACK_NS 50000ULL
#define PROCESS_DEFAULT_QUANTUM_TICKS 5U
#define SCHED_TARGET_LATENCY_TICKS 6U
#define SCHED_MIN_GRANULARITY_TICKS 1U
#define SCHED_WAKEUP_GRANULARITY_NS 4000000ULL
#define NICE_0_WEIGHT 1024ULL
#define TICK_NS (1000000000ULL / TIMER_FREQUENCY_HZ)
#define SCHED_TARGET_LATENCY_NS (SCHED_TARGET_LATENCY_TICKS * TICK_NS)
/*
 * A slice is never this long, so a sample that is did not measure one: it
 * subtracted two clocks that do not agree.
 *
 * There is one boot_tsc for the whole machine and the processors are not
 * checked against it. One whose counter runs ahead hands a task that migrated
 * onto it a delta of whole seconds, and charging that to virtual runtime puts
 * the task behind everything for ever -- runnable, never chosen, which from the
 * outside is the desktop stopping. Dropping the sample loses a slice of
 * accounting and keeps the scheduler answering.
 */
#define SCHED_MAX_SAMPLE_NS 1000000000ULL

/* Linux's well-tested geometric nice scale, rounded to integer weights. */
static const uint32_t nice_weights[40] = {
    88761, 71755, 56483, 46273, 36291, 29154, 23254, 18705, 14949, 11916,
     9548,  7620,  6100,  4904,  3906,  3121,  2501,  1991,  1586,  1277,
     1024,   820,   655,   526,   423,   335,   272,   215,   172,   137,
      110,    87,    70,    56,    45,    36,    29,    23,    18,    15,
};

static uint64_t process_weight(const struct process *process) {
    int nice = process ? process->nice : 0;
    if (nice < -20) nice = -20;
    if (nice > 19) nice = 19;
    return nice_weights[nice + 20];
}

extern void process_enter_user(uint64_t entry, uint64_t user_stack, uint64_t cr3) __attribute__((noreturn));
/* Park on the idle stack. The first releases the kernel lock on the way, for
   the paths that got here holding it; the second is for a processor that has
   never held it. */
extern void cpu_enter_idle(uint64_t idle_stack_top) __attribute__((noreturn));
extern void cpu_idle_park(uint64_t idle_stack_top) __attribute__((noreturn));
extern void kprintf(const char *fmt, ...);
extern void panic(const char *msg) __attribute__((noreturn));

#if TUNIX_DEBUG_LOGS
#define KDEBUG(...) kprintf(__VA_ARGS__)
#else
#define KDEBUG(...) do { } while (0)
#endif

static struct process *queue;
static uint64_t next_pid = 1;

/*
 * The running process, which is a different one on every processor.
 *
 * A macro rather than an accessor because it is written as often as it is
 * read, and because reading it must never be hoisted across a context switch:
 * `current` after activate_process() is the process that just came in, not the
 * one that went out, and the GS-relative load makes that literally true.
 */
#define current (cpu_current()->current)

static void signal_one_process(struct process *target, int signal_number);

static struct process_memory *memory_create(uint64_t cr3, uint64_t brk_start,
                                            uint64_t brk_end, uint64_t mmap_base) {
    struct process_memory *memory = (struct process_memory *)kmalloc(sizeof(*memory));
    if (!memory) return NULL;
    /* Zero first: the mapping table must start empty, not full of stack junk. */
    memset(memory, 0, sizeof(*memory));
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

static void areas_free(struct process_memory *memory);
static int areas_copy(struct process_memory *destination,
                      const struct process_memory *source);

static void memory_unref(struct process_memory *memory) {
    if (!memory || memory->refs == 0) return;
    memory->refs--;
    if (memory->refs != 0) return;
    areas_free(memory);
    if (memory->cr3) vmm_destroy_address_space(memory->cr3);
    kfree(memory);
}

/*
 * fork gives the child its own address space, and the shared file mappings are
 * inherited along with the pages, so the records have to be copied and their
 * files referenced again.
 */
static void memory_copy_mappings(struct process_memory *destination,
                                 const struct process_memory *source) {
    if (!destination || !source) return;
    areas_copy(destination, source);
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


/* --- the process dump ----------------------------------------------------- */

static const char *state_name(int state) {
    switch (state) {
        case PROCESS_READY:   return "ready";
        case PROCESS_RUNNING: return "run";
        case PROCESS_BLOCKED: return "block";
        case PROCESS_ZOMBIE:  return "zombie";
        case PROCESS_STOPPED: return "stop";
        default:              return "dead";
    }
}

/* The mapped object a user address falls in, for a process that is not the
   one running. Same idea as the fault reporter, which cannot be reused here
   because it only ever asks about current. */
static const char *object_at(const struct process *process, uint64_t address,
                             uint64_t *offset_out) {
    *offset_out = address;
    if (!process || !process->memory) return "?";
    for (struct vm_area *area = process->memory->areas; area; area = area->next) {
        if (address < area->start) break;
        if (address >= area->end) continue;
        *offset_out = address - area->start + area->offset;
        if (area->file && area->file->node) return area->file->node->name;
        return "anon";
    }
    return "?";
}

/*
 * Every process, what it is doing and where it stopped doing it.
 *
 * A hung desktop is a dozen processes of which one is stuck and the rest are
 * waiting on it, and telling those apart from the outside is guesswork. This
 * prints the state, whatever the process is blocked on -- a syscall being
 * retried, a futex address, a wait channel, a child -- and the user address it
 * last executed, named by the library it lands in.
 */
void process_dump_wakes(void);
static void futex_note(char kind, uint64_t address, int woken, int maximum, unsigned value);

void process_dump_all(void) {
    process_dump_wakes();
    kprintf("PROCESSES:" "\n");
    if (!queue) return;
    struct process *item = queue;
    do {
        if (item->state == PROCESS_DEAD) { item = item->next; continue; }
        uint64_t rip = item == current ? item->saved_frame.user_rip
                                       : item->saved_frame.user_rip;
        uint64_t offset = 0;
        const char *object = object_at(item, rip, &offset);
        kprintf("  %d/%d %s %s", (int)item->pid, (int)item->tgid,
                item->name, state_name(item->state));
        if (item->wait4_active) kprintf(" wait4(%d)", (int)item->wait_pid);
        if (item->io_wait_active) kprintf(" io-syscall=%d", (int)item->io_wait_syscall);
        if (item->futex_wait_active) {
            uint32_t now = 0;
            int readable = vmm_copy_from_space(item->cr3, &now,
                                               item->futex_wait_address,
                                               sizeof(now)) == 0;
            kprintf(" futex=%p want=%x now=%s%x", (void *)item->futex_wait_address,
                    (unsigned)item->futex_wait_expected, readable ? "" : "?",
                    (unsigned)now);
        }
        if (item->wait_channel) kprintf(" chan=%p", (const void *)item->wait_channel);
        if (item->syscall_rewound) kprintf(" rewound");
        kprintf(" at %s+%p" "\n", object, (void *)offset);
        item = item->next;
    } while (item != queue);
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

int process_install_file_flags(struct process *process, struct file *file,
                               int minimum_fd, uint8_t flags) {
    if (!process || !process->files || !file) return -1;
    if (minimum_fd < 0) minimum_fd = 0;
    for (int fd = minimum_fd; fd < PROCESS_MAX_FDS; fd++) {
        if (!process->files->fds[fd]) {
            process->files->fds[fd] = file;
            process->files->fd_flags[fd] = flags & PROCESS_FD_CLOEXEC;
            return fd;
        }
    }
    return -1;
}

int process_install_file(struct process *process, struct file *file, int minimum_fd) {
    return process_install_file_flags(process, file, minimum_fd, 0);
}

uint8_t process_get_fd_flags(const struct process *process, int fd) {
    if (!process || !process->files || fd < 0 || fd >= PROCESS_MAX_FDS ||
        !process->files->fds[fd]) return 0;
    return process->files->fd_flags[fd];
}

int process_set_fd_flags(struct process *process, int fd, uint8_t flags) {
    if (!process || !process->files || fd < 0 || fd >= PROCESS_MAX_FDS ||
        !process->files->fds[fd]) return -1;
    process->files->fd_flags[fd] = flags & PROCESS_FD_CLOEXEC;
    return 0;
}

int process_close_fd(struct process *process, int fd) {
    if (!process || !process->files || fd < 0 || fd >= PROCESS_MAX_FDS ||
        !process->files->fds[fd]) return -1;
    struct file *file = process->files->fds[fd];
    process->files->fds[fd] = NULL;
    process->files->fd_flags[fd] = 0;
    file_unref(file);
    return 0;
}

static struct file_table *file_table_create(void) {
    struct file_table *table = (struct file_table *)kmalloc(sizeof(*table));
    if (!table) return NULL;
    memset(table, 0, sizeof(*table));
    table->refs = 1;
    return table;
}

/* fork: the child gets its own table holding new references to the same open
   files -- descriptors diverge from here, file offsets stay shared. */
static struct file_table *file_table_clone(const struct file_table *source) {
    struct file_table *table = file_table_create();
    if (!table || !source) return table;
    for (int fd = 0; fd < PROCESS_MAX_FDS; fd++) {
        if (source->fds[fd]) {
            table->fds[fd] = source->fds[fd];
            table->fd_flags[fd] = source->fd_flags[fd];
            file_ref(table->fds[fd]);
        }
    }
    return table;
}

/* Drop this process's reference to its descriptor table; the files close only
   when the last thread sharing the table lets go. */
static void process_release_files(struct process *process) {
    if (!process || !process->files) return;
    struct file_table *table = process->files;
    process->files = NULL;
    if (--table->refs > 0) return;
    for (int fd = 0; fd < PROCESS_MAX_FDS; fd++) {
        if (table->fds[fd]) {
            struct file *file = table->fds[fd];
            table->fds[fd] = NULL;
            table->fd_flags[fd] = 0;
            file_unref(file);
        }
    }
    kfree(table);
}

/* Defined with the scheduler, below, but needed by process creation above it. */
static void fpu_save(struct process *process);
static void fpu_init_state(struct process *process);

static int allocate_kernel_stack(struct process *process) {
    uint8_t *kernel_stack = (uint8_t *)kmalloc(KERNEL_STACK_SIZE);
    if (!kernel_stack) return -1;
    process->kernel_stack_base = (uint64_t)kernel_stack;
    process->kernel_stack_top = ((uint64_t)kernel_stack + KERNEL_STACK_SIZE) & ~15ULL;
    return 0;
}

static void destroy_process_resources(struct process *process) {
    if (!process) return;
    uint64_t pid = process->pid;
#if !TUNIX_DEBUG_LOGS
    (void)pid;
#endif
    procfs_unregister_process(process->pid);
    vfs_node_unref(process->cwd);
    process->cwd = NULL;
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
    process->files->fds[0] = console;
    file_ref(console);
    process->files->fds[1] = console;
    file_ref(console);
    process->files->fds[2] = console;
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
    process->umask = 022;
    process->signal_stack_flags = SS_DISABLE;
    process->dumpable = 1;
    process->timerslack_ns = DEFAULT_TIMERSLACK_NS;
    process->files = file_table_create();
    if (!process->files) {
        kprintf("process: descriptor-table allocation failed for %s\n", path);
        kfree(process);
        return NULL;
    }
    process->cwd = vfs_root;
    vfs_node_ref(process->cwd);
    strncpy(process->name, file->name, sizeof(process->name) - 1);
    strncpy(process->exe_path, path, sizeof(process->exe_path) - 1);
    process->cr3 = vmm_create_address_space();
    if (!process->cr3) {
        kprintf("process: address-space creation failed for %s\n", path);
        process_release_files(process);
        kfree(process);
        return NULL;
    }
    process->start_time_ns = time_uptime_ns();

    const char *argv[] = {path, NULL};
    const char *envp[] = {
        "PATH=/usr/bin:/usr/sbin:/bin:/sbin",
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
        process_release_files(process);
        kfree(process);
        return NULL;
    }
    process->memory = memory_create(process->cr3, process->brk_start,
                                    process->brk_end, process->mmap_base);
    if (!process->memory) {
        vmm_destroy_address_space(process->cr3);
        process_release_files(process);
        kfree(process);
        return NULL;
    }

    fpu_init_state(process);
    if (allocate_kernel_stack(process) != 0) {
        kprintf("process: kernel stack allocation failed for %s\n", path);
        memory_unref(process->memory);
        process_release_files(process);
        kfree(process);
        return NULL;
    }
    process->saved_frame.user_rip = process->entry;
    process->saved_frame.user_rsp = process->user_stack_top;
    process->saved_frame.user_rflags = 0x202;
    install_console(process);

    enqueue(process);
    procfs_register_process(process);
    /* init starts life in the foreground of the first terminal, which is what
       makes Ctrl-C typed before anything has logged in go somewhere. */
    if (process->pid == 1) tty_set_foreground_pgid(vt_tty(1U), (int)process->pgid);
    KDEBUG("process: pid=%u path=%s entry=%p cr3=%p\n",
            (unsigned)process->pid, path, (void *)process->entry, (void *)process->cr3);
    return process;
}

struct process *process_current(void) { return current; }
uint64_t process_current_pid(void) { return current ? current->tgid : 0; }
uint64_t process_current_tid(void) { return current ? current->pid : 0; }
uint64_t process_current_ppid(void) { return current ? current->ppid : 0; }

uint32_t process_get_umask(void) {
    return current ? current->umask : 022;
}

uint32_t process_set_umask(uint32_t mask) {
    if (!current) return 022;
    uint32_t old = current->umask;
    uint32_t value = mask & 0777U;
    uint64_t group = current->tgid;
    struct process *item = queue;
    if (item) {
        do {
            if (item->tgid == group && item->state != PROCESS_DEAD) item->umask = value;
            item = item->next;
        } while (item != queue);
    }
    return old;
}

/*
 * READY and not RUNNING, which is the whole difference between one processor
 * and several. RUNNING means "a processor has this loaded right now": picking
 * it again on a second processor would run the same registers twice and let
 * two return paths write the same saved frame. Every path that gives a process
 * up marks it READY (or blocked, or dead) before it looks for the next one, so
 * nothing is lost by refusing to consider it while it is still on a processor.
 */
static int allowed_on_this_cpu(const struct process *process) {
    if (!process) return 0;
    uint64_t mask = process->affinity_mask ? process->affinity_mask : ~0ULL;
    return (mask & (1ULL << cpu_current()->index)) != 0;
}

static int runnable(const struct process *process) {
    return process && process->state == PROCESS_READY && allowed_on_this_cpu(process);
}

/*
 * The virtual runtime the runnable set has reached, which only ever goes
 * forward. next_runnable() already visits every task to find the lowest, so
 * this costs the assignment and nothing else.
 */
static uint64_t minimum_virtual_runtime;

/*
 * Where a task that has been asleep is put when it wakes.
 *
 * Virtual runtime stands still while a task is blocked and goes on rising for
 * everything that runs, so a task that slept for two seconds comes back two
 * seconds of credit ahead of the machine -- and next_runnable(), which picks
 * the lowest, then hands it the processor and nothing else until that credit is
 * spent. Measured before this, by SLEEPER in support/schedbench.c: a spinner
 * sharing a processor with a task that slept for two seconds and then wanted to
 * run went 1500 ms without being scheduled once, which is the whole of the
 * sleeper's burst.
 *
 * So a waking task is placed at the runnable set's own virtual runtime, less
 * half the target latency. The subtraction keeps a genuinely interactive task
 * ahead of the CPU-bound ones -- it wakes owed one scheduling round, which is
 * what makes it run promptly -- and the floor is what stops that debt growing
 * without limit.
 *
 * Real-time tasks are not placed: they are not chosen by virtual runtime and
 * they do not accumulate it.
 */
static void place_waking_task(struct process *process) {
    if (!process || process->rt_priority) return;
    uint64_t credit = SCHED_TARGET_LATENCY_NS / 2;
    uint64_t floor = minimum_virtual_runtime > credit ? minimum_virtual_runtime - credit : 0;
    if (process->virtual_runtime_ns < floor) process->virtual_runtime_ns = floor;
}

/*
 * Runnable again after being off the queue, which is the transition that needs
 * placing. A task that gave the processor up while still runnable -- preempted,
 * or yielding -- keeps the virtual runtime it earned and does not come through
 * here.
 */
static void wake_to_ready(struct process *process) {
    if (!process) return;
    place_waking_task(process);
    process->state = PROCESS_READY;
}

static void signal_one_process(struct process *target, int signal_number);

static void wake_expired_itimers(void) {
    if (!queue) return;

    uint64_t now = time_uptime_ns();
    struct process *item = queue;
    do {
        if (item->state != PROCESS_DEAD && item->itimer_real_deadline_ns &&
            now >= item->itimer_real_deadline_ns) {
            if (item->itimer_real_interval_ns) {
                uint64_t elapsed = now - item->itimer_real_deadline_ns;
                uint64_t periods = 1 + elapsed / item->itimer_real_interval_ns;
                uint64_t advance = periods > UINT64_MAX / item->itimer_real_interval_ns ?
                    UINT64_MAX : periods * item->itimer_real_interval_ns;
                item->itimer_real_deadline_ns = UINT64_MAX - item->itimer_real_deadline_ns < advance ?
                    UINT64_MAX : item->itimer_real_deadline_ns + advance;
            } else {
                item->itimer_real_deadline_ns = 0;
            }
            signal_one_process(item, SIGALRM);
        }
        item = item->next;
    } while (item != queue);
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
            wake_to_ready(item);
        }
        item = item->next;
    } while (item != queue);
}

/*
 * Who runs next: the highest priority anything runnable has, and round robin
 * among the threads that share it.
 *
 * Every thread was equal here until a sound mixer asked not to be. It wakes
 * every twenty milliseconds, writes for a few hundred microseconds and sleeps
 * again, and being put at the back of a queue behind a game drawing a hundred
 * frames a second is the difference between sound and stuttering. Nothing else
 * on this image asks, which is the usual shape of the thing: a real-time
 * priority is for a thread that is almost always asleep.
 *
 * That also bounds the damage. A thread that asks for a priority and then does
 * not sleep starves everything below it, exactly as it would on Linux, and
 * nothing here prevents that -- but the quantum still preempts it in favour of
 * its equals, so it cannot lock the machine against another thread at its own
 * priority.
 *
 * Two passes over a list that is walked anyway. It is short, and one of the
 * passes only reads.
 */
/* Is anything runnable with a stronger claim than this? Walks the same short
   list the scheduler walks; the answer is only interesting on a tick. */
static int higher_priority_waiting(const struct process *than) {
    if (!queue || !than) return 0;
    struct process *walk = queue;
    do {
        if (walk != than && runnable(walk) && walk->rt_priority > than->rt_priority)
            return 1;
        walk = walk->next;
    } while (walk != queue);
    return 0;
}

static int ordinary_should_preempt(const struct process *running) {
    if (!queue || !running || running->rt_priority) return 0;
    struct process *walk = queue;
    do {
        /* Signed, so that the comparison keeps meaning something if a virtual
           runtime ever goes round: these are running totals, not instants. */
        if (walk != running && runnable(walk) && !walk->rt_priority &&
            (int64_t)(walk->virtual_runtime_ns + SCHED_WAKEUP_GRANULARITY_NS -
                      running->virtual_runtime_ns) < 0)
            return 1;
        walk = walk->next;
    } while (walk != queue);
    return 0;
}

static uint32_t ordinary_slice_ticks(const struct process *selected) {
    uint64_t total_weight = 0;
    unsigned runnable_count = 0;
    struct process *walk = queue;
    if (!walk || !selected) return PROCESS_DEFAULT_QUANTUM_TICKS;
    do {
        if (runnable(walk) && !walk->rt_priority) {
            total_weight += process_weight(walk);
            runnable_count++;
        }
        walk = walk->next;
    } while (walk != queue);

    if (!total_weight) return PROCESS_DEFAULT_QUANTUM_TICKS;
    uint64_t period = SCHED_TARGET_LATENCY_TICKS;
    if (runnable_count > period) period = runnable_count;
    uint64_t ticks = period * process_weight(selected) / total_weight;
    if (ticks < SCHED_MIN_GRANULARITY_TICKS) ticks = SCHED_MIN_GRANULARITY_TICKS;
    if (ticks > SCHED_TARGET_LATENCY_TICKS) ticks = SCHED_TARGET_LATENCY_TICKS;
    return (uint32_t)ticks;
}

static struct process *next_runnable(struct process *after) {
    if (!queue) return NULL;
    wake_expired_itimers();
    wake_expired_futex_waiters();

    int best = -1;
    struct process *walk = queue;
    do {
        if (runnable(walk) && walk->rt_priority > best) best = walk->rt_priority;
        walk = walk->next;
    } while (walk != queue);
    if (best < 0) return NULL;

    struct process *candidate = after ? after->next : queue;
    struct process *start = candidate;
    if (best == 0) {
        struct process *selected = NULL;
        /*
         * The floor is taken over what is RUNNING as well as what is READY, and
         * over every processor rather than the ones this task may use.
         *
         * A running task is the one holding the lowest virtual runtime -- it is
         * running because it was lowest -- so a floor drawn from the READY set
         * alone is drawn from the tasks that were just preempted, which are the
         * highest. On one processor that hides one task and the answer is close
         * enough; on four it hides four, and the floor climbs past the running
         * set entirely. What that did was starve the thing it was meant to
         * protect: a waking task was placed *above* the spinners it was
         * competing with and then never chosen again, and the machine stopped.
         */
        uint64_t lowest = 0;
        int have_lowest = 0;
        do {
            if (!candidate->rt_priority &&
                (candidate->state == PROCESS_READY || candidate->state == PROCESS_RUNNING) &&
                (!have_lowest || (int64_t)(candidate->virtual_runtime_ns - lowest) < 0)) {
                lowest = candidate->virtual_runtime_ns;
                have_lowest = 1;
            }
            if (runnable(candidate) && !candidate->rt_priority &&
                (!selected || (int64_t)(candidate->virtual_runtime_ns -
                                        selected->virtual_runtime_ns) < 0))
                selected = candidate;
            candidate = candidate->next;
        } while (candidate != start);
        if (have_lowest && (int64_t)(lowest - minimum_virtual_runtime) > 0)
            minimum_virtual_runtime = lowest;
        return selected;
    }
    do {
        if (runnable(candidate) && candidate->rt_priority == best) return candidate;
        candidate = candidate->next;
    } while (candidate != start);
    return NULL;
}

/*
 * A thread's own scheduling, or the calling thread's when `tid` is 0.
 *
 * SCHED_FIFO is accepted and then scheduled as SCHED_RR: the difference is
 * whether the tick may take the processor away from a thread with an equal to
 * run, and answering that faithfully means a single spinning thread can stop
 * this machine with no way back in. The distinction only shows with two
 * runnable threads at one priority, neither of which ever blocks. Nothing here
 * is that, and the honest trade is a scheduler that keeps answering.
 */
static struct process *scheduling_target(uint64_t tid) {
    if (!tid) return current;
    return process_find(tid);
}

int process_set_scheduler(uint64_t tid, int policy, int rt_priority) {
    struct process *target = scheduling_target(tid);
    if (!target) return -ESRCH;

    int real_time = policy == PROCESS_SCHED_FIFO || policy == PROCESS_SCHED_RR;
    if (!real_time && policy != PROCESS_SCHED_OTHER &&
        policy != PROCESS_SCHED_BATCH && policy != PROCESS_SCHED_IDLE)
        return -EINVAL;
    if (real_time) {
        if (rt_priority < 1 || rt_priority > PROCESS_RT_PRIORITY_MAX) return -EINVAL;
        if (!cred_is_root() && rt_priority > PROCESS_RT_PRIORITY_UNPRIVILEGED_MAX)
            rt_priority = PROCESS_RT_PRIORITY_UNPRIVILEGED_MAX;
    } else {
        if (rt_priority != 0) return -EINVAL;
    }

    target->policy = policy;
    target->rt_priority = real_time ? rt_priority : 0;
    return 0;
}

int process_get_scheduler(uint64_t tid, int *policy, int *rt_priority) {
    struct process *target = scheduling_target(tid);
    if (!target) return -ESRCH;
    if (policy) *policy = target->policy;
    if (rt_priority) *rt_priority = target->rt_priority;
    return 0;
}

int process_set_nice(uint64_t tid, int nice) {
    struct process *target = scheduling_target(tid);
    if (!target) return -ESRCH;
    if (nice < -20) nice = -20;
    if (nice > 19) nice = 19;
    if (nice < target->nice && !cred_is_root()) return -EACCES;
    target->nice = nice;
    return 0;
}

int process_get_nice(uint64_t tid, int *nice) {
    struct process *target = scheduling_target(tid);
    if (!target) return -ESRCH;
    if (nice) *nice = target->nice;
    return 0;
}

static uint64_t online_cpu_mask(void) {
    unsigned cpus = smp_cpu_count();
    return cpus >= 64 ? ~0ULL : (1ULL << cpus) - 1ULL;
}

int process_set_affinity(uint64_t tid, uint64_t mask) {
    struct process *target = scheduling_target(tid);
    if (!target) return -ESRCH;
    mask &= online_cpu_mask();
    if (!mask) return -EINVAL;
    target->affinity_mask = mask;
    return 0;
}

int process_get_affinity(uint64_t tid, uint64_t *mask) {
    struct process *target = scheduling_target(tid);
    if (!target) return -ESRCH;
    if (mask) *mask = (target->affinity_mask ? target->affinity_mask : ~0ULL) &
                      online_cpu_mask();
    return 0;
}

static void fpu_save(struct process *process) {
    if (process) __asm__ volatile("fxsave64 (%0)" : : "r"(process->fpu_state) : "memory");
}

static void fpu_restore(struct process *process) {
    if (process) __asm__ volatile("fxrstor64 (%0)" : : "r"(process->fpu_state) : "memory");
}

/*
 * The register state a process starts with: what FNINIT and a default MXCSR
 * give. Built by hand rather than by running FNINIT, because doing that here
 * would disturb the registers of whichever process is currently loaded.
 */
static void fpu_init_state(struct process *process) {
    if (!process) return;
    memset(process->fpu_state, 0, sizeof(process->fpu_state));
    /* FCW: all exceptions masked, extended precision, round to nearest. */
    process->fpu_state[0] = 0x7F;
    process->fpu_state[1] = 0x03;
    /* MXCSR at offset 24, likewise with every exception masked. */
    process->fpu_state[24] = 0x80;
    process->fpu_state[25] = 0x1F;
    /* MXCSR_MASK at offset 28; the value every CPU since the PIII reports. */
    process->fpu_state[28] = 0xFF;
    process->fpu_state[29] = 0xFF;
}

static void activate_process(struct process *process) {
    /* The outgoing process's registers have to be put away before the incoming
       one's are loaded; `current` is still the outgoing process here. */
    if (current && current != process) fpu_save(current);
    current = process;
    if (!process->time_slice_ticks)
        process->time_slice_ticks = process->rt_priority
                                      ? PROCESS_DEFAULT_QUANTUM_TICKS
                                      : ordinary_slice_ticks(process);
    process->last_scheduled_ns = time_uptime_ns();
    process->state = PROCESS_RUNNING;
    set_kernel_stack(process->kernel_stack_top);
    syscall_set_kernel_stack(process->kernel_stack_top);
    /*
     * Only when it is a different one. Writing CR3 throws away every
     * translation this processor had cached, and two threads of one process
     * share a cr3 -- so switching between them used to flush the TLB for
     * nothing. Measured: a ping-pong between two threads of one process cost
     * exactly what the same ping-pong between two processes cost, which is what
     * it looks like when the page tables are reloaded either way.
     *
     * The comparison is safe because this field is written every time the
     * processor changes what it has loaded: go_idle() zeroes it, and every
     * other path through here sets it. It can never name a space this processor
     * is not actually running on, which is the only way a skipped reload could
     * leave stale translations behind.
     */
    if (cpu_current()->address_space != process->cr3) {
        vmm_activate(process->cr3);
        cpu_current()->address_space = process->cr3;
    }
    wrmsr(IA32_FS_BASE, process->fs_base);
    fpu_restore(process);
}

static void save_interrupt_context(struct syscall_frame *destination,
                                   const struct interrupt_frame *source) {
    destination->r15 = source->r15;
    destination->r14 = source->r14;
    destination->r13 = source->r13;
    destination->r12 = source->r12;
    destination->rbp = source->rbp;
    destination->rbx = source->rbx;
    destination->r9 = source->r9;
    destination->r8 = source->r8;
    destination->r10 = source->r10;
    destination->rdx = source->rdx;
    destination->rsi = source->rsi;
    destination->rdi = source->rdi;
    destination->rax = source->rax;
    destination->rcx = source->rcx;
    destination->r11 = source->r11;
    destination->user_rip = source->rip;
    destination->user_rflags = source->rflags;
    destination->user_rsp = source->rsp;
}

static void load_interrupt_context(struct interrupt_frame *destination,
                                   const struct syscall_frame *source) {
    destination->ds = 0x1b;
    destination->r15 = source->r15;
    destination->r14 = source->r14;
    destination->r13 = source->r13;
    destination->r12 = source->r12;
    destination->r11 = source->r11;
    destination->r10 = source->r10;
    destination->r9 = source->r9;
    destination->r8 = source->r8;
    destination->rbp = source->rbp;
    destination->rdi = source->rdi;
    destination->rsi = source->rsi;
    destination->rdx = source->rdx;
    destination->rcx = source->rcx;
    destination->rbx = source->rbx;
    destination->rax = source->rax;
    destination->rip = source->user_rip;
    destination->cs = 0x23;
    destination->rflags = source->user_rflags | 0x2ULL;
    destination->rsp = source->user_rsp;
    destination->ss = 0x1b;
}

static int switch_to_next(struct syscall_frame *frame, struct process *after) {
    struct process *next = next_runnable(after);
    if (!next) return -1;
    *frame = next->saved_frame;
    activate_process(next);
    return 0;
}

/*
 * Nothing left to run here. The processor drops the process it was holding,
 * goes back to the kernel address space so the process's own can be torn down,
 * and parks on its idle stack with interrupts on. Its next timer tick is what
 * wakes it up with work.
 *
 * The kernel stack it was using is abandoned rather than unwound: everything
 * worth keeping is already in the process, which is the same reason a blocking
 * syscall can switch away mid-call.
 */
static void go_idle(void) __attribute__((noreturn));
static void go_idle(void) {
    klock_note(KLOCK_NOTE_IDLE);
    if (current) {
        fpu_save(current);
        current = NULL;
    }
    vmm_activate(vmm_kernel_cr3());
    cpu_current()->address_space = 0;
    /* Both kernel-stack pointers name the process that has just been let go
       of, whose stack can be reaped and its pages handed back at any moment.
       Nothing should reach them while the processor is idle -- an entry from
       user mode is the only thing that uses them, and there is no user to
       enter from -- but leaving a freed address in a register the processor
       reads is not worth the argument. */
    set_kernel_stack(cpu_current()->idle_stack_top);
    syscall_set_kernel_stack(cpu_current()->idle_stack_top);
    cpu_enter_idle(cpu_current()->idle_stack_top);
}

void process_start_first(void) {
    klock_note(KLOCK_NOTE_FIRST_RUN);
    kernel_lock();
    struct process *first = next_runnable(NULL);
    /* Not a failure: on a machine with several processors another one may
       have taken the first process already, and this one simply has nothing
       yet. Worth a line even so -- it is the difference between a machine
       whose init is running somewhere else and one whose init never ran. */
    if (!first) {
        kprintf("TUNIX: cpu %u has nothing to run\n", cpu_current()->index);
        go_idle();
    }
    activate_process(first);
    uint64_t entry = first->entry;
    uint64_t stack = first->user_stack_top;
    uint64_t cr3 = first->cr3;
    kernel_unlock();
    kprintf("TUNIX: cpu %u entering user mode\n", cpu_current()->index);
    process_enter_user(entry, stack, cr3);
    panic("process_enter_user returned");
}

/* Every processor but the first arrives here, with nothing to run yet. */
void process_run_idle(void) {
    cpu_idle_park(cpu_current()->idle_stack_top);
}

/*
 * Map one more stack page for the current process. Called from the page-fault
 * handler before the fault is turned into a signal, so a program that walks
 * past the initial mapping simply gets more stack instead of dying.
 *
 * Only the address range is checked, not its distance from rsp. Linux is
 * stricter, which lets it catch a wild pointer that happens to land in the
 * stack window; here a compiler that writes below rsp before adjusting it --
 * which does happen without stack-clash protection -- matters more than that
 * diagnostic, so the simpler rule wins.
 *
 * Returns 1 when a page was mapped and the faulting instruction should be
 * retried, 0 when the fault was not a stack growth and must be handled.
 */
/* --- the address-space map ---------------------------------------------- */

static struct vm_area **area_list(void) {
    return current && current->memory ? &current->memory->areas : NULL;
}

static struct vm_area *area_alloc(uint64_t start, uint64_t end,
                                  uint64_t page_flags, uint32_t kind,
                                  struct file *file, uint64_t offset) {
    struct vm_area *area = (struct vm_area *)kmalloc(sizeof(*area));
    if (!area) return NULL;
    area->start = start;
    area->end = end;
    area->page_flags = page_flags;
    area->kind = kind;
    area->file = file;
    area->offset = offset;
    area->next = NULL;
    if (file) file_ref(file);
    if ((kind & VM_FILE_PAGES) && file) vfs_map_ref(file->node);
    return area;
}

static void area_free(struct vm_area *area) {
    if (!area) return;
    /* Before the file reference, not after: dropping the last one can free the
       descriptor, and the node is reached through it. */
    if ((area->kind & VM_FILE_PAGES) && area->file) vfs_map_unref(area->file->node);
    if (area->file) file_unref(area->file);
    kfree(area);
}

/* Splitting keeps the offset pointing at the same place in the backing object,
   which is the whole reason the offset is recorded. */
static struct vm_area *area_split_at(struct vm_area *area, uint64_t cut) {
    struct vm_area *tail = area_alloc(cut, area->end, area->page_flags,
                                      area->kind, area->file,
                                      area->offset + (cut - area->start));
    if (!tail) return NULL;
    area->end = cut;
    tail->next = area->next;
    area->next = tail;
    return tail;
}

/* Insert keeping the list sorted and non-overlapping; the caller has already
   cleared whatever used to live in the range. */
static void area_insert(struct vm_area **list, struct vm_area *area) {
    struct vm_area **link = list;
    while (*link && (*link)->start < area->start) link = &(*link)->next;
    area->next = *link;
    *link = area;
}

int process_map_area(uint64_t start, uint64_t end, uint64_t page_flags,
                     uint32_t kind, struct file *file, uint64_t offset) {
    struct vm_area **list = area_list();
    if (!list || start >= end) return -1;
    process_unmap_area(start, end);
    struct vm_area *area = area_alloc(start, end, page_flags, kind, file, offset);
    if (!area) return -1;
    area_insert(list, area);
    return 0;
}

/*
 * Remove [start, end) from the map. An area the range falls inside splits in
 * two, which is the case munmap and mprotect hit most; if the tail cannot be
 * allocated the area is simply truncated, so the map never describes memory
 * that is not there.
 */
void process_unmap_area(uint64_t start, uint64_t end) {
    struct vm_area **list = area_list();
    if (!list || start >= end) return;

    struct vm_area **link = list;
    while (*link) {
        struct vm_area *area = *link;
        if (area->end <= start || area->start >= end) {
            link = &area->next;
            continue;
        }
        if (start <= area->start && end >= area->end) {
            *link = area->next;
            area_free(area);
            continue;
        }
        if (start > area->start && end < area->end) {
            struct vm_area *tail = area_split_at(area, end);
            area->end = start;
            link = tail ? &tail->next : &area->next;
            continue;
        }
        if (start > area->start) {
            area->end = start;
        } else {
            area->offset += end - area->start;
            area->start = end;
        }
        link = &area->next;
    }
}

/* mprotect over part of an area splits it, so the new permissions apply to
   exactly the range asked for. */
void process_protect_area(uint64_t start, uint64_t end, uint64_t page_flags) {
    struct vm_area **list = area_list();
    if (!list || start >= end) return;

    struct vm_area **link = list;
    while (*link) {
        struct vm_area *area = *link;
        if (area->end <= start || area->start >= end) {
            link = &area->next;
            continue;
        }
        /* Trim whatever lies outside the range into an area of its own, then
           look at what is left rather than stepping over it: a cut at the end
           leaves the head exactly equal to the range, and stepping over it was
           how an mprotect that shrank an area from the tail came to change no
           permissions at all. glibc's malloc does exactly that -- it reserves
           an arena PROT_NONE and makes the first pages of it readable and
           writable -- so the first write into a new arena died on a page the
           kernel had committed read-only.

           Each pass either splits (and the head then falls outside the range,
           or matches it exactly) or advances, so this still terminates. */
        if (area->start < start || area->end > end) {
            uint64_t cut = area->start < start ? start : end;
            if (!area_split_at(area, cut)) {
                link = &area->next;
            }
            continue;
        }
        area->page_flags = page_flags;
        link = &area->next;
    }
}

int process_area_range_free(uint64_t start, uint64_t end) {
    struct vm_area **list = area_list();
    if (!list || start >= end) return 0;
    for (struct vm_area *area = *list; area; area = area->next) {
        if (area->start >= end) break;
        if (area->end > start) return 0;
    }
    return 1;
}

/* Walk the gaps between areas rather than probing address by address. */
int process_find_free_range(uint64_t start, uint64_t length, uint64_t *base_out) {
    struct vm_area **list = area_list();
    if (!list || !length || !base_out) return -1;
    uint64_t base = start;
    for (struct vm_area *area = *list; area; area = area->next) {
        if (area->end <= base) continue;
        if (area->start >= base && area->start - base >= length) break;
        base = area->end;
        if (base >= USER_ADDRESS_LIMIT) return -1;
    }
    if (length > USER_ADDRESS_LIMIT - base) return -1;
    *base_out = base;
    return 0;
}

struct vm_area *process_find_area(uint64_t address) {
    struct vm_area **list = area_list();
    if (!list) return NULL;
    for (struct vm_area *area = *list; area; area = area->next) {
        if (address < area->start) return NULL;
        if (address < area->end) return area;
    }
    return NULL;
}

int process_commit_area(uint64_t fault_address) {
    if (!current || current->state != PROCESS_RUNNING || !current->cr3) return 0;
    uint64_t page = fault_address & ~4095ULL;
    struct vm_area *area = process_find_area(page);
    if (!area || !(area->kind & VM_ANONYMOUS)) return 0;
    /* Mapped already, and the caller only gets here for a not-present fault:
       another thread of this process committed the page on another processor
       between the fault and now. Retrying the instruction is all that is
       left to do. */
    if (vmm_translate(current->cr3, page, NULL, NULL) == 0) return 1;

    uint64_t physical = (uint64_t)pmm_alloc_page();
    if (!physical) return 0;
    memset(vmm_phys_to_virt(physical), 0, 4096);
    if (vmm_map_page_in(current->cr3, page, physical, area->page_flags) != 0) {
        pmm_free_page((void *)physical);
        return 0;
    }
    return 1;
}

static void areas_free(struct process_memory *memory) {
    if (!memory) return;
    struct vm_area *area = memory->areas;
    while (area) {
        struct vm_area *next = area->next;
        area_free(area);
        area = next;
    }
    memory->areas = NULL;
}

static int areas_copy(struct process_memory *destination,
                      const struct process_memory *source) {
    struct vm_area **link = &destination->areas;
    for (const struct vm_area *area = source->areas; area; area = area->next) {
        struct vm_area *copy =
            area_alloc(area->start, area->end, area->page_flags, area->kind,
                       area->file, area->offset);
        if (!copy) {
            areas_free(destination);
            return -1;
        }
        *link = copy;
        link = &copy->next;
    }
    return 0;
}

int process_grow_user_stack(uint64_t fault_address) {
    if (!current || current->state != PROCESS_RUNNING || !current->cr3) return 0;
    if (fault_address >= USER_STACK_TOP || fault_address < USER_STACK_LIMIT) return 0;

    uint64_t page = fault_address & ~4095ULL;
    uint64_t existing_physical = 0;
    uint64_t existing_flags = 0;
    /* Same race as process_commit_area: a sibling thread on another processor
       grew the stack between this fault and its handler. */
    if (vmm_translate(current->cr3, page, &existing_physical, &existing_flags) == 0)
        return 1;

    uint64_t physical = (uint64_t)pmm_alloc_page();
    if (!physical) return 0;
    memset(vmm_phys_to_virt(physical), 0, 4096);
    if (vmm_map_page_in(current->cr3, page, physical,
                        PAGE_PRESENT | PAGE_WRITE | PAGE_USER) != 0) {
        pmm_free_page((void *)physical);
        return 0;
    }
    return 1;
}

/*
 * First write to a page that fork shared instead of copying. Returns non-zero
 * when the page was made private and writable, so the faulting instruction can
 * simply be retried; zero means this was not a copy-on-write page and the
 * caller should continue on to the signal path.
 */
int process_handle_cow_fault(uint64_t fault_address) {
    if (!current || current->state != PROCESS_RUNNING || !current->cr3) return 0;
    return vmm_handle_cow_fault(current->cr3, fault_address & ~4095ULL) == 0;
}

/*
 * A CPU exception raised in user mode is that process's fault, not the
 * kernel's. Turn it into a signal so the offending program dies on its own
 * instead of taking the machine down; only a fault raised in kernel mode is
 * genuinely unrecoverable. Returns non-zero when the fault was handled.
 */
int process_fault_from_interrupt(struct interrupt_frame *frame, int signal_number) {
    if (!frame || (frame->cs & 3U) != 3U || !current ||
        current->state != PROCESS_RUNNING) return 0;

    process_account_runtime();
    save_interrupt_context(&current->saved_frame, frame);
    struct syscall_frame resume = current->saved_frame;

    (void)process_send_signal((int64_t)current->pid, signal_number);
    /* Delivers the signal: redirects to a handler if one is installed, or
       terminates the process and switches away when the action is default. */
    process_prepare_user_return(&resume);
    if (!current || current->state != PROCESS_RUNNING) return 1;
    current->saved_frame = resume;
    load_interrupt_context(frame, &resume);
    return 1;
}

/*
 * A tick that arrived in the idle loop. The interrupt came from kernel mode,
 * but in long mode the processor pushes SS:RSP for those too, so the frame can
 * be replaced wholesale with a process's and the iretq lands in user mode --
 * which is how an idle processor picks up work without any context to unwind.
 */
static void resume_from_idle(struct interrupt_frame *frame) {
    struct process *next = next_runnable(NULL);
    if (!next) return;
    activate_process(next);
    struct syscall_frame resume = next->saved_frame;
    process_prepare_user_return(&resume);
    if (!current || current->state != PROCESS_RUNNING) go_idle();
    current->saved_frame = resume;
    load_interrupt_context(frame, &resume);
}

void process_timer_interrupt(struct interrupt_frame *frame) {
    if (!frame) return;
    if (!current) {
        resume_from_idle(frame);
        return;
    }
    if ((frame->cs & 3U) != 3U || current->state != PROCESS_RUNNING) return;

    process_account_runtime();
    save_interrupt_context(&current->saved_frame, frame);

    struct syscall_frame resume = current->saved_frame;
    if (current->time_slice_ticks) current->time_slice_ticks--;
    /*
     * A priority is worth little without this. The quantum is five ticks, so a
     * thread that wakes with a claim on the processor would otherwise wait out
     * whatever is running -- up to twenty milliseconds, which is a whole audio
     * period. Measured: worst-case wake-up went from 44 ms to 20 ms when
     * priorities arrived, and the twenty was this. Giving the tick permission
     * to take the processor away for a higher priority is what makes the rest
     * of it mean something.
     */
    if (!current->time_slice_ticks || higher_priority_waiting(current) ||
        ordinary_should_preempt(current) || !allowed_on_this_cpu(current)) {
        struct process *preempted = current;
        preempted->state = PROCESS_READY;
        struct process *next = next_runnable(preempted);
        if (next && next != preempted) {
            preempted->involuntary_switches++;
            resume = next->saved_frame;
            activate_process(next);
        } else {
            if (!allowed_on_this_cpu(preempted)) go_idle();
            /* Before the state changes, not after. ordinary_slice_ticks()
               divides the period among the tasks that are READY, so a task
               already marked RUNNING is counted out of its own share -- which
               is what made the two callers of it disagree about the same
               task. activate_process() has always done it in this order. */
            preempted->time_slice_ticks = preempted->rt_priority
                                           ? PROCESS_DEFAULT_QUANTUM_TICKS
                                           : ordinary_slice_ticks(preempted);
            preempted->state = PROCESS_RUNNING;
            current = preempted;
        }
    }

    /* Timer return is also a safe point for signals sent to CPU-bound tasks. */
    process_prepare_user_return(&resume);
    if (!current || current->state != PROCESS_RUNNING) return;
    current->saved_frame = resume;
    load_interrupt_context(frame, &resume);
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
    int status = child->termination_signal
        ? (child->termination_signal & 0x7F)
        : ((child->exit_status & 0xFF) << 8);
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
    if (parent->wait4_active && parent->state == PROCESS_BLOCKED &&
        child_matches(child, parent, parent->wait_pid)) {
        if (store_wait_status(parent, child, parent->wait_status_user) == 0) parent->saved_frame.rax = child->pid;
        else parent->saved_frame.rax = (uint64_t)-(int64_t)EINVAL;
        parent->wait4_active = 0;
        parent->wait_pid = 0;
        parent->wait_status_user = 0;
        parent->wait_options = 0;
        wake_to_ready(parent);
        child->state = PROCESS_DEAD;
    }
}

static int notify_parent_of_job_change(struct process *child, int wait_flag, int status) {
    struct process *parent = find_parent(child);
    if (!parent) return 0;

    parent->signal_pending |= signal_bit(SIGCHLD);
    if (!parent->wait4_active || parent->state != PROCESS_BLOCKED ||
        !child_matches(child, parent, parent->wait_pid) ||
        !(parent->wait_options & wait_flag)) return 0;

    if (store_job_status(parent, status, parent->wait_status_user) == 0)
        parent->saved_frame.rax = child->pid;
    else
        parent->saved_frame.rax = (uint64_t)-(int64_t)EINVAL;
    parent->wait4_active = 0;
    parent->wait_pid = 0;
    parent->wait_status_user = 0;
    parent->wait_options = 0;
    wake_to_ready(parent);
    return 1;
}

struct linux_robust_list_head_user {
    uint64_t list_next;
    int64_t futex_offset;
    uint64_t list_op_pending;
};

static void robust_wake_address(struct process *process, uint64_t address) {
    if (!process || (address & 3U) || address >= USER_ADDRESS_LIMIT) return;
    uint32_t value;
    if (vmm_copy_from_space(process->cr3, &value, address, sizeof(value)) != 0) return;
    if ((value & FUTEX_TID_MASK) != (uint32_t)process->pid) return;
    value = (value & ~FUTEX_TID_MASK) | FUTEX_OWNER_DIED;
    if (vmm_copy_to_space(process->cr3, address, &value, sizeof(value)) != 0) return;
    (void)process_futex_wake(address, 1, FUTEX_BITSET_MATCH_ANY);
}

static int robust_futex_address(uint64_t entry, int64_t offset, uint64_t *address) {
    if (!address) return -1;
    if (offset < 0) {
        uint64_t amount = (uint64_t)(-offset);
        if (entry < amount) return -1;
        *address = entry - amount;
    } else {
        uint64_t amount = (uint64_t)offset;
        if (entry > USER_ADDRESS_LIMIT - amount) return -1;
        *address = entry + amount;
    }
    return 0;
}

static void process_handle_robust_list(struct process *process) {
    if (!process || !process->robust_list_head ||
        process->robust_list_length != sizeof(struct linux_robust_list_head_user)) return;

    uint64_t head_address = process->robust_list_head;
    struct linux_robust_list_head_user head;
    if (vmm_copy_from_space(process->cr3, &head, head_address, sizeof(head)) != 0) return;

    uint64_t entry = head.list_next;
    for (unsigned count = 0; entry && entry != head_address && count < ROBUST_LIST_LIMIT; count++) {
        uint64_t next;
        if (vmm_copy_from_space(process->cr3, &next, entry, sizeof(next)) != 0) break;
        uint64_t futex_address;
        if (robust_futex_address(entry, head.futex_offset, &futex_address) == 0)
            robust_wake_address(process, futex_address);
        entry = next;
    }

    if (head.list_op_pending && head.list_op_pending != head_address) {
        uint64_t futex_address;
        if (robust_futex_address(head.list_op_pending, head.futex_offset, &futex_address) == 0)
            robust_wake_address(process, futex_address);
    }
    process->robust_list_head = 0;
    process->robust_list_length = 0;
}

static void notify_children_of_parent_death(struct process *parent) {
    if (!parent || !queue) return;
    struct process *item = queue;
    do {
        if (item != parent && item->ppid == parent->pid && item->state != PROCESS_DEAD) {
            int signal_number = item->pdeath_signal;
            item->ppid = 0;
            if (signal_number > 0) signal_one_process(item, signal_number);
        }
        item = item->next;
    } while (item != queue);
}

/* Defined with exit_group, which wants the same teardown. */
static void terminate_sibling_threads(int status);

static void process_exit_from_signal(struct syscall_frame *frame, int signal_number) {
    /* Before the teardown, while the frame still describes where it died. */
    if (current && current->pid == 1)
        kprintf("TUNIX: init killed by signal %d at rip %p rsp %p\n",
                signal_number, (void *)(frame ? frame->user_rip : 0),
                (void *)(frame ? frame->user_rsp : 0));
    if (current) current->termination_signal = signal_number;
    /* The signal was aimed at the process; the thread it landed on is an
       accident of scheduling, and taking only that one down leaves the rest
       running with the process's files still open. */
    terminate_sibling_threads(128 + signal_number);
    if (current) current->is_thread = 0;
    process_exit_from_syscall(frame, 128 + signal_number);
}

void process_exit_from_syscall(struct syscall_frame *frame, int status) {
    if (!current || !frame) panic("process: exit without current process");
    struct process *exiting = current;
    /*
     * Init leaving is the end of the machine, and it has to say so. There is
     * nothing left to boot into, no parent to report to, and every processor
     * goes idle -- which from the outside is a black screen and a cursor, and
     * indistinguishable from a kernel that hung.
     */
    if (!exiting->is_thread && exiting->pid == 1) {
        kprintf("TUNIX: init exited, status %d\n", status);
        panic("init exited");
    }
    exiting->exit_status = status;
    exiting->state = PROCESS_ZOMBIE;
    process_handle_robust_list(exiting);
    notify_children_of_parent_death(exiting);
    if (exiting->clear_child_tid_user) {
        uint64_t clear_address = exiting->clear_child_tid_user;
        uint32_t zero = 0;
        (void)vmm_copy_to_space(exiting->cr3, clear_address, &zero, sizeof(zero));
        exiting->clear_child_tid_user = 0;
        (void)process_futex_wake(clear_address, 1, FUTEX_BITSET_MATCH_ANY);
    }
    process_release_files(exiting);
    /* A process that drove a virtual terminal has to let go of it here: the
       display could otherwise stay owed to a program that no longer exists,
       and a switch it was asked to release would never be answered. */
    if (!exiting->is_thread)
        vt_process_exited(exiting->pid,
                          /* Only the leader's exit ends the session; a child
                             shell leaving is not the login going away. */
                          exiting->sid == exiting->pid ? exiting->sid : 0);
    if (exiting->is_thread) exiting->state = PROCESS_DEAD;
    else notify_parent_of_exit(exiting);
    KDEBUG("process: pid=%u exited status=%d\n", (unsigned)exiting->pid, status);

    if (switch_to_next(frame, exiting) != 0) go_idle();
}

int64_t process_fork_from_syscall(struct syscall_frame *frame) {
    if (!current || !frame) return -EINVAL;
    struct process *parent = current;
    struct process *child = (struct process *)kmalloc(sizeof(*child));
    if (!child) return -EINVAL;
    memset(child, 0, sizeof(*child));

    child->pid = next_pid++;
    child->tgid = child->pid;
    child->ppid = parent->pid;
    child->pgid = parent->pgid;
    child->sid = parent->sid;
    child->state = PROCESS_READY;
    child->cwd = parent->cwd;
    /* Counted like the fds below: the child keeps the directory alive on its
       own, so the parent chdir'ing or the directory being removed cannot leave
       the child pointing at freed memory. */
    vfs_node_ref(child->cwd);
    child->controlling_pty = parent->controlling_pty;
    child->umask = parent->umask;
    child->cred = parent->cred;
    /* Scheduling is inherited, as it is on Linux: a thread the mixer starts
       has the same claim on the processor its parent had. */
    child->policy = parent->policy;
    child->rt_priority = parent->rt_priority;
    child->nice = parent->nice;
    child->virtual_runtime_ns = parent->virtual_runtime_ns;
    child->affinity_mask = parent->affinity_mask;
    child->signal_stack_pointer = parent->signal_stack_pointer;
    child->signal_stack_size = parent->signal_stack_size;
    child->signal_stack_flags = parent->signal_stack_flags;
    child->dumpable = parent->dumpable;
    child->no_new_privs = parent->no_new_privs;
    child->timerslack_ns = parent->timerslack_ns;
    child->thp_disable = parent->thp_disable;
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
    /* Shared file mappings are inherited with the pages, so the records that
       describe them have to come along. */
    memory_copy_mappings(child->memory, parent->memory);
    /* The child continues from where the parent is, x87/SSE registers included.
       The parent is the running process, so its live registers have to be put
       into its save area before they can be copied out of it. */
    fpu_save(parent);
    memcpy(child->fpu_state, parent->fpu_state, sizeof(child->fpu_state));
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
    if (allocate_kernel_stack(child) != 0) {
        memory_unref(child->memory);
        kfree(child);
        return -EINVAL;
    }

    child->saved_frame = *frame;
    child->saved_frame.rax = 0;
    child->signal_blocked = parent->signal_blocked;
    memcpy(child->signal_actions, parent->signal_actions, sizeof(child->signal_actions));

    child->files = file_table_clone(parent->files);
    if (!child->files) {
        memory_unref(child->memory);
        kfree((void *)child->kernel_stack_base);
        kfree(child);
        return -EAGAIN;
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
    vfs_node_ref(child->cwd);
    child->controlling_pty = parent->controlling_pty;
    child->umask = parent->umask;
    child->cred = parent->cred;
    /* Scheduling is inherited, as it is on Linux: a thread the mixer starts
       has the same claim on the processor its parent had. */
    child->policy = parent->policy;
    child->rt_priority = parent->rt_priority;
    child->nice = parent->nice;
    child->virtual_runtime_ns = parent->virtual_runtime_ns;
    child->affinity_mask = parent->affinity_mask;
    child->signal_stack_flags = SS_DISABLE;
    child->dumpable = parent->dumpable;
    child->no_new_privs = parent->no_new_privs;
    child->timerslack_ns = parent->timerslack_ns;
    child->thp_disable = parent->thp_disable;
    strncpy(child->name, parent->name, sizeof(child->name) - 1);
    strncpy(child->exe_path, parent->exe_path, sizeof(child->exe_path) - 1);
    child->memory = parent->memory;
    memory_ref(child->memory);
    sync_memory_view(child);
    /* A new thread starts with the creating thread's floating-point state, for
       the same reason fork does. */
    fpu_save(parent);
    memcpy(child->fpu_state, parent->fpu_state, sizeof(child->fpu_state));
    child->entry = parent->entry;
    child->user_stack_top = child_stack;
    child->fs_base = (flags & 0x00080000ULL) ? tls : parent->fs_base;
    child->start_time_ns = time_uptime_ns();
    child->cmdline_length = parent->cmdline_length;
    memcpy(child->cmdline, parent->cmdline, sizeof(child->cmdline));
    if (allocate_kernel_stack(child) != 0) {
        memory_unref(child->memory);
        kfree(child);
        return -EINVAL;
    }

    child->saved_frame = *frame;
    child->saved_frame.rax = 0;
    child->saved_frame.user_rsp = child_stack;
    child->signal_blocked = parent->signal_blocked;
    memcpy(child->signal_actions, parent->signal_actions, sizeof(child->signal_actions));

    /* CLONE_FILES: threads share one descriptor table, they do not copy it.
       An fd any thread opens (or closes) from here on is immediately visible
       to every sibling. */
    child->files = parent->files;
    child->files->refs++;

    uint32_t tid = (uint32_t)child->pid;
    if ((flags & 0x00100000ULL) && parent_tid_user &&
        vmm_copy_to_space(parent->cr3, parent_tid_user, &tid, sizeof(tid)) != 0) {
        process_release_files(child);
        memory_unref(child->memory);
        kfree((void *)child->kernel_stack_base);
        kfree(child);
        return -EFAULT;
    }
    if ((flags & (0x01000000ULL | 0x00200000ULL)) && child_tid_user) {
        if ((flags & 0x01000000ULL) &&
            vmm_copy_to_space(child->cr3, child_tid_user, &tid, sizeof(tid)) != 0) {
            process_release_files(child);
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
                           uint32_t expected, int64_t timeout_ns,
                           uint32_t bitset) {
    if (!current || !frame || (address & 3U) || address >= USER_ADDRESS_LIMIT)
        return -EINVAL;
    uint32_t value = 0;
    if (vmm_copy_from_space(current->cr3, &value, address, sizeof(value)) != 0)
        return -EFAULT;
    if (value != expected) { futex_note('A', address, 0, 0, value); return -EAGAIN; }
    if (timeout_ns == 0) return -ETIMEDOUT;

    struct process *waiting = current;
    waiting->saved_frame = *frame;
    waiting->saved_frame.rax = 0;
    waiting->state = PROCESS_BLOCKED;
    waiting->futex_wait_active = 1;
    waiting->futex_wait_address = address;
    waiting->futex_wait_expected = expected;
    waiting->futex_wait_bitset = bitset;
    futex_note('W', address, 0, 0, expected);
    waiting->futex_wait_deadline_ns = timeout_ns < 0 ? UINT64_MAX :
        time_uptime_ns() + (uint64_t)timeout_ns;
    /* Unlike wait4, EAGAIN here is a true answer and not a lie: it is what a
       futex whose value no longer matches returns, and the caller re-reads and
       tries again. It costs a spin rather than a park, which is why the two
       are not the same. */
    if (switch_to_next(frame, waiting) != 0) {
        waiting->state = PROCESS_RUNNING;
        waiting->futex_wait_active = 0;
        waiting->futex_wait_address = 0;
        waiting->futex_wait_deadline_ns = 0;
        return -EAGAIN;
    }
    return 0;
}

int process_sleep_on(struct syscall_frame *frame, const void *channel) {
    if (!current || !frame || !channel) return -EAGAIN;
    struct process *waiting = current;
    waiting->saved_frame = *frame;
    waiting->state = PROCESS_BLOCKED;
    waiting->wait_channel = channel;
    if (switch_to_next(frame, waiting) != 0) {
        /*
         * Nothing else to run: park the processor rather than refuse to
         * sleep. It used to refuse, so that a wakeup which never arrived
         * became a slow loop instead of a hang -- but "nothing else to run"
         * is what an idle machine looks like, so the safety net was paid for
         * continuously. Measured on a machine doing nothing at all, with a
         * shell at a prompt on two terminals: three of its four processors
         * spinning here, for ever.
         *
         * Parking is safe now because the tick wakes the general io channel
         * (see process_wake_io), so a sleeper whose own wakeup was missed is
         * still re-tested a few hundred times a second. The processor halts
         * until the next interrupt, which is what an idle processor should
         * cost.
         *
         * This does not return: the kernel stack it was using belongs to the
         * process that has just gone to sleep, and everything worth keeping
         * was written into saved_frame above.
         */
        go_idle();
    }
    return 0;
}

/* The address is the channel; the object is never read. */
static const char io_wait_token;

const void *process_io_wait_channel(void) { return &io_wait_token; }

int process_wake_io(void) { return process_wake_all(&io_wait_token); }

/* Guarded: a shared-mode pipe read wakes whoever was waiting for space, and
   that walks the queue every other processor may also be walking. */
int process_wake_all(const void *channel) {
    oplock_enter();
    int woken = process_wake_all_locked(channel);
    oplock_leave();
    return woken;
}

static int process_wake_all_locked(const void *channel) {
    if (!queue || !channel) return 0;
    int woken = 0;
    struct process *item = queue;
    do {
        /* Anything that made one wait channel ready may have made a poll()
           ready too, and the poller cannot know which queue to have joined.
           Releasing the io waiters alongside costs nothing -- they re-test --
           and saves them waiting for the next tick to notice. */
        if (item->state == PROCESS_BLOCKED &&
            (item->wait_channel == channel || item->wait_channel == &io_wait_token)) {
            item->wait_channel = NULL;
            wake_to_ready(item);
            woken++;
        }
        item = item->next;
    } while (item != queue);
    return woken;
}

struct wake_record { uint64_t address; int woken; int pid; int max; char kind; unsigned value; };
#define WAKE_RING 160
static struct wake_record wake_ring[WAKE_RING];
static unsigned wake_ring_next;

static void futex_note(char kind, uint64_t address, int woken, int maximum, unsigned value) {
    /* A thread that cannot sleep re-enters the wait forever; recording each
       attempt would push everything else out of the ring. */
    unsigned last = (wake_ring_next + WAKE_RING - 1U) % WAKE_RING;
    if (wake_ring[last].address == address && wake_ring[last].kind == kind &&
        wake_ring[last].pid == (current ? (int)current->pid : 0) &&
        wake_ring[last].value == value)
        return;
    wake_ring[wake_ring_next].kind = kind;
    wake_ring[wake_ring_next].address = address;
    wake_ring[wake_ring_next].woken = woken;
    wake_ring[wake_ring_next].max = maximum;
    wake_ring[wake_ring_next].value = value;
    wake_ring[wake_ring_next].pid = current ? (int)current->pid : 0;
    wake_ring_next = (wake_ring_next + 1U) % WAKE_RING;
}

void process_dump_wakes(void) {
    kprintf("FUTEX LOG:\n");
    for (unsigned i = 0; i < WAKE_RING; i++) {
        unsigned slot = (wake_ring_next + i) % WAKE_RING;
        if (!wake_ring[slot].address) continue;
        kprintf("  %c %p pid=%d n=%d/%d val=%x\n", wake_ring[slot].kind,
                (void *)wake_ring[slot].address, wake_ring[slot].pid,
                wake_ring[slot].woken, wake_ring[slot].max, wake_ring[slot].value);
    }
}

int process_futex_wake(uint64_t address, int maximum, uint32_t bitset) {
    if (!current || !queue || maximum <= 0 || !bitset) return 0;
    int woken = 0;
    struct process *item = queue;
    do {
        if (item->state == PROCESS_BLOCKED && item->futex_wait_active &&
            item->memory == current->memory &&
            item->futex_wait_address == address &&
            (item->futex_wait_bitset & bitset)) {
            item->futex_wait_active = 0;
            item->futex_wait_address = 0;
            item->futex_wait_deadline_ns = 0;
            item->saved_frame.rax = 0;
            wake_to_ready(item);
            woken++;
            if (woken >= maximum) break;
        }
        item = item->next;
    } while (item != queue);
    futex_note('K', address, woken, maximum, 0);
    return woken;
}

/* End every *other* thread sharing the caller's thread group.
 *
 * A process is its thread group: exit_group says so outright, and a fatal
 * signal means the same thing even though it arrives at one thread. Leaving
 * the siblings running is not a tidiness problem -- they hold the process's
 * open files, so nothing the process had open is ever closed. That is how
 * killing the browser left five WebKitWebProcess threads alive at a quarter
 * gigabyte each, and their sockets open, so the helper processes on the far
 * end never saw the end-of-file that tells them to exit either.
 */
/*
 * Take the rest of the thread group down with this thread.
 *
 * A sibling that is PROCESS_RUNNING is loaded on another processor and is
 * executing its own user code right now. Tearing it down from here -- closing
 * its files, marking it dead so it can be reaped and its address space freed --
 * pulls the ground out from under a thread that is still standing on it, and
 * the first thing that processor does with it turns into a kernel panic rather
 * than a signal. On one processor the case could not arise: a sibling was
 * always ready or blocked, never actually executing.
 *
 * So such a thread is told to leave instead of being made to. It reads the flag
 * on its own next return to user mode -- its own timer tick at the latest --
 * and runs the same exit path it would have run for itself.
 */
/*
 * CLONE_SIGHAND is mandatory for a thread here, which means the handler table
 * belongs to the thread group and not to one thread. Threads are given a copy
 * of it when they are created, so a later sigaction has to be written through
 * to the siblings as well.
 *
 * musl is what makes this load-bearing: setuid and friends in a threaded
 * process install a SIGSYNCCALL handler and then signal every other thread
 * with it, and a sibling still holding SIG_DFL for a real-time signal takes
 * the whole process down instead of answering.
 */
void process_set_sigaction(int signal_number,
                           const struct tunix_sigaction *action) {
    if (!current || signal_number < 1 || signal_number > TUNIX_NSIG) return;
    current->signal_actions[signal_number - 1] = *action;
    if (!queue) return;
    uint64_t group = current->tgid;
    struct process *item = queue;
    do {
        if (item != current && item->tgid == group && item->state != PROCESS_DEAD)
            item->signal_actions[signal_number - 1] = *action;
        item = item->next;
    } while (item != queue);
}

static void terminate_sibling_threads(int status) {
    if (!current || !queue) return;
    uint64_t group = current->tgid;
    struct process *item = queue;
    do {
        if (item != current && item->tgid == group && item->state != PROCESS_DEAD) {
            item->exit_status = status;
            if (item->state == PROCESS_RUNNING) {
                item->group_exit_pending = 1;
                item = item->next;
                continue;
            }
            process_handle_robust_list(item);
            if (item->clear_child_tid_user) {
                uint64_t clear_address = item->clear_child_tid_user;
                uint32_t zero = 0;
                (void)vmm_copy_to_space(item->cr3, clear_address, &zero, sizeof(zero));
                item->clear_child_tid_user = 0;
                (void)process_futex_wake(clear_address, 1, FUTEX_BITSET_MATCH_ANY);
            }
            item->state = PROCESS_DEAD;
            process_release_files(item);
        }
        item = item->next;
    } while (item != queue);
}

void process_exit_group_from_syscall(struct syscall_frame *frame, int status) {
    if (!current || !frame) panic("process: exit_group without current process");
    terminate_sibling_threads(status);
    current->is_thread = 0;
    process_exit_from_syscall(frame, status);
}

int64_t process_exec_from_syscall(struct syscall_frame *frame, const char *path,
                                  const char *const argv[], const char *const envp[],
                                  const struct vfs_node *credential_source) {
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
    /* A different program entirely: it must not inherit the old one's
       floating-point registers, so reset them and load the reset state,
       because these are the live registers of the running process. */
    fpu_init_state(current);
    fpu_restore(current);
    current->user_stack_top = image.user_stack_top;
    current->brk_start = image.brk_start;
    current->brk_end = image.brk_end;
    current->mmap_base = image.mmap_base;
    current->fs_base = 0;
    current->signal_stack_pointer = 0;
    current->signal_stack_size = 0;
    current->signal_stack_flags = SS_DISABLE;
    current->robust_list_head = 0;
    current->robust_list_length = 0;
    cred_apply_exec(&current->cred, credential_source, current->no_new_privs);
    /* A program that gained privileges must not be inspectable by the identity
       that started it. */
    current->dumpable = (current->cred.euid == current->cred.uid &&
                         current->cred.egid == current->cred.gid);
    strncpy(current->name, file->name, sizeof(current->name) - 1);
    strncpy(current->exe_path, path, sizeof(current->exe_path) - 1);
    set_process_cmdline(current, path, argv);
    for (int sig = 0; sig < TUNIX_NSIG; sig++) {
        if (current->signal_actions[sig].handler != SIG_IGN) memset(&current->signal_actions[sig], 0, sizeof(current->signal_actions[sig]));
    }
    current->signal_pending = 0;
    current->in_signal = 0;
    for (int fd = 0; fd < PROCESS_MAX_FDS; fd++) {
        if (current->files->fds[fd] && (current->files->fd_flags[fd] & PROCESS_FD_CLOEXEC))
            process_close_fd(current, fd);
    }

    memset(frame, 0, sizeof(*frame));
    frame->user_rip = current->entry;
    frame->user_rsp = current->user_stack_top;
    frame->user_rflags = 0x202;
    vmm_activate(new_cr3);
    /* The processor is looking at a different space now, and the record of
       which space that is decides who gets told when a mapping in it changes.
       Left stale, this processor is invisible to the next flush. */
    cpu_current()->address_space = new_cr3;
    wrmsr(IA32_FS_BASE, 0);
    if (old_memory) memory_unref(old_memory);
    else vmm_destroy_address_space(old_cr3);
    KDEBUG("process: pid=%u exec %s\n", (unsigned)current->pid, path);
    return 0;
}

int64_t process_waitpid_from_syscall(struct syscall_frame *frame, int64_t pid,
                                     uint64_t status_user, int options) {
    options &= ~(WNOTHREAD | WALLCHILDREN | WCLONE);
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
    parent->wait4_active = 1;
    parent->wait_pid = pid;
    parent->wait_status_user = status_user;
    parent->wait_options = options;
    /*
     * Nothing else to run *here* is not the same as nothing else to run. The
     * child this caller is waiting for is very likely the reason: it is
     * PROCESS_RUNNING on another processor, which makes it exactly the thing
     * next_runnable() must not pick. Reporting ECHILD then is a lie -- the
     * child exists and is executing -- and a shell believes it: bash reads it
     * as "the job is gone" and runs the next command over the top of one that
     * is still going.
     *
     * So the processor parks instead. The child's exit wakes the sleeper.
     */
    if (switch_to_next(frame, parent) != 0) go_idle();
    return 0;
}

/*
 * waitid(2), the non-blocking core. dinit's event library (dasynq) reaps every
 * service exit through waitid(P_ALL, 0, &info, WNOHANG | WEXITED); with wait4
 * alone a service manager stays permanently blind to its children -- services
 * hang in STARTING and their processes pile up as zombies.
 *
 * The siginfo_t written back is the x86_64 Linux layout: 128 bytes, the
 * SIGCHLD union member starting at offset 16. Only the fields a waiter can
 * consume are filled; the rest stay zero.
 *
 * Returns 0 with the record stored (or, for WNOHANG with nothing ready, with
 * si_pid zero, which is how Linux reports it), -EAGAIN when the caller should
 * block and retry, or a negative errno.
 */
struct waitid_siginfo {
    int32_t si_signo;
    int32_t si_errno;
    int32_t si_code;
    int32_t pad0;
    int32_t si_pid;
    uint32_t si_uid;
    int32_t si_status;
    uint8_t pad1[128 - 28];
};

#define CLD_EXITED 1
#define CLD_KILLED 2
#define CLD_STOPPED 5
#define CLD_CONTINUED 6

int64_t process_waitid_from_syscall(int64_t pid_spec, uint64_t info_user,
                                    int options) {
    if (!current || !info_user) return -EINVAL;
    struct process *parent = current;
    struct waitid_siginfo info;
    memset(&info, 0, sizeof(info));

    int has_child = 0;
    struct process *item = queue;
    if (item) {
        do {
            if (child_matches(item, parent, pid_spec)) {
                has_child = 1;
                if ((options & WEXITED) && item->state == PROCESS_ZOMBIE) {
                    info.si_signo = SIGCHLD;
                    info.si_pid = (int32_t)item->pid;
                    if (item->termination_signal) {
                        info.si_code = CLD_KILLED;
                        info.si_status = item->termination_signal & 0x7F;
                    } else {
                        info.si_code = CLD_EXITED;
                        info.si_status = item->exit_status & 0xFF;
                    }
                    if (vmm_copy_to_space(parent->cr3, info_user, &info,
                                          sizeof(info)) != 0) return -EFAULT;
                    item->state = PROCESS_DEAD;
                    return 0;
                }
                if ((options & WSTOPPED) && item->state == PROCESS_STOPPED &&
                    !item->stop_reported) {
                    info.si_signo = SIGCHLD;
                    info.si_pid = (int32_t)item->pid;
                    info.si_code = CLD_STOPPED;
                    info.si_status = item->stop_signal & 0xFF;
                    if (vmm_copy_to_space(parent->cr3, info_user, &info,
                                          sizeof(info)) != 0) return -EFAULT;
                    item->stop_reported = 1;
                    return 0;
                }
                if ((options & WCONTINUED) && item->continued_pending) {
                    info.si_signo = SIGCHLD;
                    info.si_pid = (int32_t)item->pid;
                    info.si_code = CLD_CONTINUED;
                    info.si_status = SIGCONT;
                    if (vmm_copy_to_space(parent->cr3, info_user, &info,
                                          sizeof(info)) != 0) return -EFAULT;
                    item->continued_pending = 0;
                    return 0;
                }
            }
            item = item->next;
        } while (item != queue);
    }
    if (!has_child) return -ECHILD;
    if (options & WNOHANG) {
        /* "si_pid and si_signo are set to zero" -- waitid(2). The whole
           record is zeroed, which satisfies both the letter and dasynq. */
        if (vmm_copy_to_space(parent->cr3, info_user, &info, sizeof(info)) != 0)
            return -EFAULT;
        return 0;
    }
    return -EAGAIN;
}

static void signal_one_process(struct process *target, int signal_number) {
    if (signal_number == 0) return;
    if (signal_number == SIGKILL && target->state == PROCESS_STOPPED)
        wake_to_ready(target);
    if (signal_number == SIGCONT && target->state == PROCESS_STOPPED) {
        wake_to_ready(target);
        target->continued_pending = 1;
        target->stop_reported = 0;
        target->signal_pending &= ~(signal_bit(SIGSTOP) | signal_bit(SIGTSTP) |
                                    signal_bit(SIGTTIN) | signal_bit(SIGTTOU));
        if (notify_parent_of_job_change(target, WCONTINUED, 0xFFFF))
            target->continued_pending = 0;
    }
    target->signal_pending |= signal_bit(signal_number);
    if (target->state == PROCESS_BLOCKED && signal_number != SIGCHLD) {
        target->futex_wait_active = 0;
        target->futex_wait_address = 0;
        target->futex_wait_deadline_ns = 0;
        /* Do NOT stamp -EINTR over a syscall that block_and_retry() rewound to
         * be retried: its saved rax still holds the syscall *number*, which the
         * rewound %rip will re-issue on resume. process_prepare_user_return()
         * decides that frame's fate when the signal is actually taken -- restart
         * it (SA_RESTART) or convert it to -EINTR and step past the syscall.
         * Clobbering rax here would feed -EINTR (-4) back as a syscall number,
         * which lands in the ENOSYS default and surfaces to userspace as a
         * bogus "read error: Function not implemented". Non-rewound sleepers
         * (wait4, etc.) have no retry %rip, so -EINTR is their correct return. */
        if (!target->syscall_rewound)
            target->saved_frame.rax = (uint64_t)-(int64_t)EINTR;
        target->wait4_active = 0;
        target->wait_channel = NULL;
        target->wait_pid = 0;
        target->wait_status_user = 0;
        target->wait_options = 0;
        wake_to_ready(target);
    }
}

/* An unprivileged sender may only signal processes of its own identity. */
static int may_signal(const struct process *target) {
    const struct credentials *sender = &current->cred;
    if (sender->euid == 0) return 1;
    return sender->uid == target->cred.uid || sender->uid == target->cred.suid ||
           sender->euid == target->cred.uid || sender->euid == target->cred.suid;
}

/* Remember who a kill(2) came from, so the handler's siginfo can say. */
static void record_sender(struct process *target, int signal_number) {
    if (signal_number < 1 || signal_number > TUNIX_NSIG) return;
    target->signal_user_sent |= signal_bit(signal_number);
    target->signal_sender_pid[signal_number - 1] = (uint32_t)current->pid;
    target->signal_sender_uid[signal_number - 1] = current->cred.uid;
}

static int send_signal(int64_t pid, int signal_number, int checked) {
    if (signal_number < 0 || signal_number > TUNIX_NSIG) return -EINVAL;
    /*
     * A kill(2) is judged against the caller, and a pid of zero means the
     * caller's own group, so both of those need one. A signal the kernel sends
     * on its own behalf needs neither -- and frequently has no caller at all.
     *
     * Ctrl+Alt+F2 is the case that matters. It arrives as an interrupt, the
     * processor that takes it is usually idle, and an idle processor has no
     * current process: the release signal to whoever owns the terminal was
     * refused with EINVAL and the screen stayed where it was. Which looked
     * exactly like a compositor refusing to let go.
     */
    if ((checked || pid == 0) && !current) return -EINVAL;
    if (pid > 0) {
        struct process *target = process_find((uint64_t)pid);
        if (!target) return -ESRCH;
        if (checked && !may_signal(target)) return -EPERM;
        signal_one_process(target, signal_number);
        if (checked) record_sender(target, signal_number);
        return 0;
    }

    uint64_t group = pid == 0 ? current->pgid : (uint64_t)(-pid);
    int delivered = 0;
    int refused = 0;
    if (!queue) return -ESRCH;
    struct process *target = queue;
    do {
        int match = pid == -1 ? target->pid != 1 : target->pgid == group;
        if (match && target->state != PROCESS_DEAD) {
            if (checked && !may_signal(target)) refused = 1;
            else {
                signal_one_process(target, signal_number);
                if (checked) record_sender(target, signal_number);
                delivered = 1;
            }
        }
        target = target->next;
    } while (target != queue);
    if (delivered) return 0;
    return refused ? -EPERM : -ESRCH;
}

int process_send_signal(int64_t pid, int signal_number) {
    return send_signal(pid, signal_number, 0);
}

int process_send_signal_checked(int64_t pid, int signal_number) {
    return send_signal(pid, signal_number, 1);
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

static int on_signal_stack(const struct process *process, uint64_t user_rsp) {
    if (!process || process->signal_stack_flags == SS_DISABLE ||
        !process->signal_stack_size) return 0;
    uint64_t base = process->signal_stack_pointer;
    uint64_t limit = base + process->signal_stack_size;
    return user_rsp >= base && user_rsp < limit;
}

void process_prepare_user_return(struct syscall_frame *frame) {
    if (!current || !frame || current->state != PROCESS_RUNNING) return;
    /* Before signals, and regardless of in_signal: the group is already gone
       and there is nothing left for a handler to run on. */
    if (current->group_exit_pending) {
        current->group_exit_pending = 0;
        process_exit_from_syscall(frame, current->exit_status);
        return;
    }
    if (current->in_signal) return;
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
        (action->handler == SIG_DFL &&
         (signal_number == SIGTSTP || signal_number == SIGTTIN ||
          signal_number == SIGTTOU))) {
        current->stop_signal = signal_number;
        current->stop_reported = 0;
        current->continued_pending = 0;
        current->state = PROCESS_STOPPED;
        current->stop_reported = notify_parent_of_job_change(
            current, WUNTRACED, ((signal_number & 0xFF) << 8) | 0x7F);
        /* Same as wait4: with nothing else ready here, the answer is to park
           the processor, not to keep running a process that has been told to
           stop. */
        if (switch_to_next(frame, current) != 0) go_idle();
        return;
    }
    if (action->handler == SIG_DFL || signal_number == SIGKILL) {
        process_exit_from_signal(frame, signal_number);
        return;
    }
    if (action->handler >= USER_ADDRESS_LIMIT || action->restorer >= USER_ADDRESS_LIMIT) {
        process_exit_from_signal(frame, SIGSEGV);
        return;
    }

    /* A frame rewound to retry a blocking syscall must observe the signal:
       return -EINTR at the instruction after the syscall so the retry does
       not silently restart, unless the handler asked for SA_RESTART. */
    if (current->syscall_rewound) {
        current->syscall_rewound = 0;
        if (!(action->flags & SA_RESTART)) {
            frame->user_rip += 2U;
            frame->rax = (uint64_t)-(int64_t)EINTR;
            current->io_wait_active = 0;
            current->io_wait_syscall = 0;
            current->io_wait_deadline_ns = 0;
        }
    }

    uint64_t stack_top = frame->user_rsp;
    if ((action->flags & SA_ONSTACK) &&
        current->signal_stack_flags != SS_DISABLE &&
        !on_signal_stack(current, frame->user_rsp)) {
        stack_top = current->signal_stack_pointer + current->signal_stack_size;
    }
    /*
     * SA_SIGINFO handlers are called as (signo, siginfo_t *, void *), and they
     * dereference the second argument. sudo's event loop is one of them: with
     * only rdi set it read its siginfo through whatever the interrupted frame
     * left in rsi and faulted on every signal it was sent. Both structures are
     * built on the user stack; the context is zeroed rather than filled,
     * because nothing here inspects a machine context and reading zeroes beats
     * reading rubbish.
     */
    uint64_t area = stack_top & ~15ULL;
    uint64_t siginfo_address = 0;
    uint64_t context_address = 0;
    if (action->flags & SA_SIGINFO) {
        uint8_t zeros[SIGNAL_CONTEXT_SIZE];
        memset(zeros, 0, sizeof(zeros));
        area -= SIGNAL_CONTEXT_SIZE;
        context_address = area;
        if (vmm_copy_to_space(current->cr3, context_address, zeros, SIGNAL_CONTEXT_SIZE) != 0) {
            process_exit_from_signal(frame, SIGSEGV);
            return;
        }
        int32_t info[SIGNAL_SIGINFO_SIZE / 4];
        memset(info, 0, sizeof(info));
        info[0] = signal_number;
        int from_user = (current->signal_user_sent & bit) != 0;
        info[2] = from_user ? SI_USER : SI_KERNEL;
        /* si_pid and si_uid, at offsets 16 and 20 of the x86_64 siginfo_t.
           A handler that only wants to know it was signalled ignores these;
           one that has to tell its children apart cannot. LightDM is the
           second kind: the X server reports itself ready by raising SIGUSR1,
           and LightDM looks the sender up by pid to decide which server it
           was -- so a zero here left it waiting forever for a signal it had
           already been sent. */
        if (from_user) {
            info[4] = (int32_t)current->signal_sender_pid[signal_number - 1];
            info[5] = (int32_t)current->signal_sender_uid[signal_number - 1];
        }
        area -= SIGNAL_SIGINFO_SIZE;
        siginfo_address = area;
        if (vmm_copy_to_space(current->cr3, siginfo_address, info, SIGNAL_SIGINFO_SIZE) != 0) {
            process_exit_from_signal(frame, SIGSEGV);
            return;
        }
        area &= ~15ULL;
    }
    current->signal_user_sent &= ~bit;
    current->signal_sender_pid[signal_number - 1] = 0;
    current->signal_sender_uid[signal_number - 1] = 0;

    uint64_t new_rsp = area - 8;
    if (vmm_copy_to_space(current->cr3, new_rsp, &action->restorer, sizeof(action->restorer)) != 0) {
        process_exit_from_signal(frame, SIGSEGV);
        return;
    }
    current->signal_saved_frame = *frame;
    current->signal_saved_mask = current->signal_blocked;
    current->signal_blocked |= action->mask | bit;
    current->in_signal = 1;
    frame->user_rsp = new_rsp;
    frame->user_rip = action->handler;
    frame->rdi = (uint64_t)signal_number;
    frame->rsi = siginfo_address;
    frame->rdx = context_address;
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
    if (now >= current->last_scheduled_ns) {
        uint64_t elapsed = now - current->last_scheduled_ns;
        if (elapsed <= SCHED_MAX_SAMPLE_NS) {
            current->runtime_ns += elapsed;
            if (!current->rt_priority) {
                uint64_t weight = process_weight(current);
                current->virtual_runtime_ns += elapsed * NICE_0_WEIGHT / weight;
            }
        }
    }
    current->last_scheduled_ns = now;
}

uint64_t process_runtime_ns(const struct process *process) {
    if (!process) return 0;
    uint64_t runtime = process->runtime_ns;
    /* RUNNING now means "loaded on some processor", not necessarily this one,
       and the slice in flight counts wherever it is being spent. */
    if (process->state == PROCESS_RUNNING && process->last_scheduled_ns) {
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
