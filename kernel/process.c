#include <stddef.h>
#include <stdint.h>
#include "include/build_config.h"
#include "include/cred.h"
#include "include/elf.h"
#include "include/eventfs.h"
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
#define IA32_KERNEL_GS_BASE 0xC0000102U
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
#define SCHED_MAX_SAMPLE_NS 1000000000ULL

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
static int reap_pending;
static int zombie_memory_pending;

#define current (cpu_current()->current)

static void signal_one_process(struct process *target, int signal_number);

static struct process_memory *memory_create(uint64_t cr3, uint64_t brk_start,
                                            uint64_t brk_end, uint64_t mmap_base) {
    struct process_memory *memory = (struct process_memory *)kmalloc(sizeof(*memory));
    if (!memory) return NULL;
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

static void release_zombie_memory(void) {
    if (!queue) return;
    int skipped = 0;
    struct process *item = queue;
    do {
        if (item->state == PROCESS_ZOMBIE && item->memory) {
            if (item == current) {
                skipped = 1;
            } else {
                memory_unref(item->memory);
                item->memory = NULL;
                item->cr3 = 0;
            }
        }
        item = item->next;
    } while (item != queue);
    zombie_memory_pending = skipped;
}

void process_reap_deferred(void) {
    if (zombie_memory_pending) release_zombie_memory();
    if (!reap_pending) return;

    int skipped = 0;
    for (;;) {
        if (!queue) break;

        struct process *previous = NULL;
        struct process *item = queue;
        struct process *victim = NULL;
        do {
            if (item->state == PROCESS_DEAD) {
                if (item == current) skipped = 1;
                else { victim = item; break; }
            }
            previous = item;
            item = item->next;
        } while (item != queue);

        if (!victim) break;
        if (victim->next == victim) {
            queue = NULL;
        } else {
            if (!previous) {
                previous = queue;
                while (previous->next != queue) previous = previous->next;
            }
            previous->next = victim->next;
            if (queue == victim) queue = victim->next;
        }
        victim->next = NULL;
        destroy_process_resources(victim);
    }
    reap_pending = skipped;
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

static void set_exe_path(struct process *process, struct vfs_node *file,
                         const char *path) {
    if (file && vfs_node_path(file, process->exe_path,
                              sizeof(process->exe_path)) == 0)
        return;
    strncpy(process->exe_path, path, sizeof(process->exe_path) - 1);
    process->exe_path[sizeof(process->exe_path) - 1] = '\0';
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
    set_exe_path(process, file, path);
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
    eventfs_emit_process_exec(process->cred.euid, process->pid, process->name);
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

static int allowed_on_this_cpu(const struct process *process) {
    if (!process) return 0;
    uint64_t mask = process->affinity_mask ? process->affinity_mask : ~0ULL;
    return (mask & (1ULL << cpu_current()->index)) != 0;
}

static int runnable(const struct process *process) {
    return process && process->state == PROCESS_READY && allowed_on_this_cpu(process);
}

static uint64_t minimum_virtual_runtime;

static void place_waking_task(struct process *process) {
    if (!process || process->rt_priority) return;
    uint64_t credit = SCHED_TARGET_LATENCY_NS / 2;
    uint64_t floor = minimum_virtual_runtime > credit ? minimum_virtual_runtime - credit : 0;
    if (process->virtual_runtime_ns < floor) process->virtual_runtime_ns = floor;
}

static void mark_dead(struct process *process) {
    if (!process) return;
    process->state = PROCESS_DEAD;
    reap_pending = 1;
}

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
            item->futex_wait_key = 0;
            item->futex_wait_deadline_ns = 0;
            item->saved_frame.rax = (uint64_t)-(int64_t)ETIMEDOUT;
            wake_to_ready(item);
        }
        item = item->next;
    } while (item != queue);
}

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

static void fpu_init_state(struct process *process) {
    if (!process) return;
    memset(process->fpu_state, 0, sizeof(process->fpu_state));
    process->fpu_state[0] = 0x7F;
    process->fpu_state[1] = 0x03;
    process->fpu_state[24] = 0x80;
    process->fpu_state[25] = 0x1F;
    process->fpu_state[28] = 0xFF;
    process->fpu_state[29] = 0xFF;
}

static void activate_process(struct process *process) {
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
    if (cpu_current()->address_space != process->cr3) {
        vmm_activate(process->cr3);
        cpu_current()->address_space = process->cr3;
    }
    wrmsr(IA32_FS_BASE, process->fs_base);
    wrmsr(IA32_KERNEL_GS_BASE, process->gs_base);
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

static void go_idle(void) __attribute__((noreturn));
static void go_idle(void) {
    klock_note(KLOCK_NOTE_IDLE);
    if (current) {
        fpu_save(current);
        current = NULL;
    }
    vmm_activate(vmm_kernel_cr3());
    cpu_current()->address_space = 0;
    set_kernel_stack(cpu_current()->idle_stack_top);
    syscall_set_kernel_stack(cpu_current()->idle_stack_top);
    cpu_enter_idle(cpu_current()->idle_stack_top);
}

void process_start_first(void) {
    klock_note(KLOCK_NOTE_FIRST_RUN);
    kernel_lock();
    go_idle();
}

void process_run_idle(void) {
    cpu_idle_park(cpu_current()->idle_stack_top);
}

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
    if ((kind & VM_FILE_PAGES) && file) {
        vfs_map_ref(file->node);
        if (page_flags & PAGE_WRITE)
            vfs_map_write_ref(file->node, offset, end - start);
    }
    return area;
}

static void area_free(struct vm_area *area) {
    if (!area) return;
    if ((area->kind & VM_FILE_PAGES) && area->file) {
        if (area->page_flags & PAGE_WRITE) vfs_map_write_unref(area->file->node);
        vfs_map_unref(area->file->node);
    }
    if (area->file) file_unref(area->file);
    kfree(area);
}

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

int process_sync_file_areas(uint64_t start, uint64_t end) {
    struct vm_area **list = area_list();
    if (!list) return 0;
    int covered = 0;
    for (struct vm_area *area = *list; area; area = area->next) {
        if (area->start >= end) break;
        if (area->end <= start) continue;
        covered = 1;
        if ((area->kind & VM_FILE_PAGES) && area->file &&
            (area->page_flags & PAGE_WRITE))
            vfs_flush_mapped(area->file->node);
    }
    return covered;
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

static int reclaim_or_kill(void) {
    if (vfs_reclaim_file_data(vfs_root)) return 1;

    struct process *victim = NULL;
    uint64_t worst = 0;
    struct process *item = queue;
    if (item) do {
        if (item->pid > 1 && !item->is_thread &&
            item->state != PROCESS_ZOMBIE && item->state != PROCESS_DEAD &&
            item->cr3 && !item->group_exit_pending) {
            uint64_t pages = vmm_count_user_pages(item->cr3);
            if (pages > worst) { worst = pages; victim = item; }
        }
        item = item->next;
    } while (item != queue);

    if (!victim) return 0;
    kprintf("OOM: killing pid=%u (%s), %u MiB resident\n",
            (unsigned)victim->pid, victim->name,
            (unsigned)(worst / 256U));
    (void)process_send_signal((int64_t)victim->pid, SIGKILL);
    return 0;
}

static uint64_t alloc_user_page(void) {
    uint64_t physical = (uint64_t)pmm_alloc_page();
    if (!physical && reclaim_or_kill()) physical = (uint64_t)pmm_alloc_page();
    return physical;
}

int process_commit_area(uint64_t fault_address) {
    if (!current || current->state != PROCESS_RUNNING || !current->cr3) return 0;
    uint64_t page = fault_address & ~4095ULL;
    struct vm_area *area = process_find_area(page);
    if (!area || !(area->kind & VM_ANONYMOUS)) return 0;
    if (vmm_translate(current->cr3, page, NULL, NULL) == 0) return 1;

    uint64_t physical = alloc_user_page();
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
    if (vmm_translate(current->cr3, page, &existing_physical, &existing_flags) == 0)
        return 1;

    uint64_t physical = alloc_user_page();
    if (!physical) return 0;
    memset(vmm_phys_to_virt(physical), 0, 4096);
    if (vmm_map_page_in(current->cr3, page, physical,
                        PAGE_PRESENT | PAGE_WRITE | PAGE_USER) != 0) {
        pmm_free_page((void *)physical);
        return 0;
    }
    return 1;
}

int process_handle_cow_fault(uint64_t fault_address) {
    if (!current || current->state != PROCESS_RUNNING || !current->cr3) return 0;
    return vmm_handle_cow_fault(current->cr3, fault_address & ~4095ULL) == 0;
}

int process_signal_has_handler(int signal_number) {
    if (!current || signal_number < 1 || signal_number > TUNIX_NSIG) return 0;
    uint64_t handler = current->signal_actions[signal_number - 1].handler;
    return handler != SIG_DFL && handler != SIG_IGN;
}

int process_fault_from_interrupt(struct interrupt_frame *frame, int signal_number) {
    if (!frame || (frame->cs & 3U) != 3U || !current ||
        current->state != PROCESS_RUNNING) return 0;

    process_account_runtime();
    save_interrupt_context(&current->saved_frame, frame);
    struct syscall_frame resume = current->saved_frame;

    const char *type = signal_number == SIGSEGV ? "segv" :
                       signal_number == SIGILL ? "ill" :
                       signal_number == SIGBUS ? "bus" :
                       signal_number == SIGFPE ? "fpe" : "signal";
    eventfs_emit_process_fault(current->cred.euid, current->pid, type,
                               current->name);

    (void)process_send_signal((int64_t)current->pid, signal_number);
    process_prepare_user_return(&resume);
    if (!current || current->state != PROCESS_RUNNING) return 1;
    current->saved_frame = resume;
    load_interrupt_context(frame, &resume);
    return 1;
}

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
            preempted->time_slice_ticks = preempted->rt_priority
                                           ? PROCESS_DEFAULT_QUANTUM_TICKS
                                           : ordinary_slice_ticks(preempted);
            preempted->state = PROCESS_RUNNING;
            current = preempted;
        }
    }

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
        mark_dead(child);
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
        mark_dead(child);
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
    (void)process_futex_wake(address, 1, FUTEX_BITSET_MATCH_ANY, 1);
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
        struct process *child = item;
        item = item->next;
        if (child == parent || child->ppid != parent->pid ||
            child->state == PROCESS_DEAD) continue;
        int signal_number = child->pdeath_signal;
        child->ppid = 1;
        if (signal_number > 0) signal_one_process(child, signal_number);
        if (child->state == PROCESS_ZOMBIE) notify_parent_of_exit(child);
    } while (item != queue);
}

static void terminate_sibling_threads(int status);

static void process_exit_from_signal(struct syscall_frame *frame, int signal_number) {
    if (current && current->pid == 1)
        kprintf("TUNIX: init killed by signal %d at rip %p rsp %p\n",
                signal_number, (void *)(frame ? frame->user_rip : 0),
                (void *)(frame ? frame->user_rsp : 0));
    if (current) current->termination_signal = signal_number;
    terminate_sibling_threads(128 + signal_number);
    if (current) current->is_thread = 0;
    process_exit_from_syscall(frame, 128 + signal_number);
}

void process_exit_from_syscall(struct syscall_frame *frame, int status) {
    if (!current || !frame) panic("process: exit without current process");
    struct process *exiting = current;
    if (!exiting->is_thread && exiting->pid == 1) {
        kprintf("TUNIX: init exited, status %d\n", status);
        panic("init exited");
    }
    eventfs_emit_process_exit(exiting->cred.euid, exiting->pid, status);
    exiting->exit_status = status;
    exiting->state = PROCESS_ZOMBIE;
    if (exiting->memory) zombie_memory_pending = 1;
    process_handle_robust_list(exiting);
    notify_children_of_parent_death(exiting);
    if (exiting->clear_child_tid_user) {
        uint64_t clear_address = exiting->clear_child_tid_user;
        uint32_t zero = 0;
        (void)vmm_copy_to_space(exiting->cr3, clear_address, &zero, sizeof(zero));
        exiting->clear_child_tid_user = 0;
        (void)process_futex_wake(clear_address, 1, FUTEX_BITSET_MATCH_ANY, 1);
    }
    process_release_files(exiting);
    if (!exiting->is_thread)
        vt_process_exited(exiting->pid,
                          exiting->sid == exiting->pid ? exiting->sid : 0);
    if (exiting->is_thread) mark_dead(exiting);
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
    vfs_node_ref(child->cwd);
    child->controlling_pty = parent->controlling_pty;
    child->umask = parent->umask;
    child->cred = parent->cred;
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
    memory_copy_mappings(child->memory, parent->memory);
    fpu_save(parent);
    memcpy(child->fpu_state, parent->fpu_state, sizeof(child->fpu_state));
    child->entry = parent->entry;
    child->user_stack_top = parent->user_stack_top;
    child->brk_start = parent_brk_start;
    child->brk_end = parent_brk_end;
    child->mmap_base = parent_mmap_base;
    child->fs_base = parent->fs_base;
    child->gs_base = parent->gs_base;
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
    eventfs_emit_process_fork(parent->cred.euid, parent->pid, child->pid);
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
    fpu_save(parent);
    memcpy(child->fpu_state, parent->fpu_state, sizeof(child->fpu_state));
    child->entry = parent->entry;
    child->user_stack_top = child_stack;
    child->fs_base = (flags & 0x00080000ULL) ? tls : parent->fs_base;
    child->gs_base = parent->gs_base;
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

static uint64_t futex_shared_key(uint64_t address) {
    if (!current || !current->cr3) return 0;
    uint64_t physical = 0, flags = 0;
    if (vmm_translate(current->cr3, address, &physical, &flags) != 0) return 0;
    if (!(flags & PAGE_SHARED)) return 0;
    return physical;
}

int64_t process_futex_wait(struct syscall_frame *frame, uint64_t address,
                           uint32_t expected, int64_t timeout_ns,
                           uint32_t bitset, int shared) {
    if (!current || !frame || (address & 3U) || address >= USER_ADDRESS_LIMIT)
        return -EINVAL;
    uint32_t value = 0;
    if (vmm_copy_from_space(current->cr3, &value, address, sizeof(value)) != 0)
        return -EFAULT;
    if (value != expected) { futex_note('A', address, 0, 0, value); return -EAGAIN; }
    if (timeout_ns == 0) return -ETIMEDOUT;
    if (process_signal_interrupts_wait()) return -EINTR;

    struct process *waiting = current;
    waiting->saved_frame = *frame;
    waiting->saved_frame.rax = 0;
    waiting->state = PROCESS_BLOCKED;
    waiting->futex_wait_active = 1;
    waiting->futex_wait_address = address;
    waiting->futex_wait_key = shared ? futex_shared_key(address) : 0;
    waiting->futex_wait_expected = expected;
    waiting->futex_wait_bitset = bitset;
    futex_note('W', address, 0, 0, expected);
    waiting->futex_wait_deadline_ns = timeout_ns < 0 ? UINT64_MAX :
        time_uptime_ns() + (uint64_t)timeout_ns;
    if (switch_to_next(frame, waiting) != 0) {
        waiting->state = PROCESS_RUNNING;
        waiting->futex_wait_active = 0;
        waiting->futex_wait_address = 0;
        waiting->futex_wait_key = 0;
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
        go_idle();
    }
    return 0;
}

static const char io_wait_token;

const void *process_io_wait_channel(void) { return &io_wait_token; }

int process_wake_io(void) { return process_wake_all(&io_wait_token); }

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

int process_futex_wake(uint64_t address, int maximum, uint32_t bitset, int shared) {
    if (!current || !queue || maximum <= 0 || !bitset) return 0;
    uint64_t key = shared ? futex_shared_key(address) : 0;
    int woken = 0;
    struct process *item = queue;
    do {
        int named = (key && item->futex_wait_key == key) ||
                    (item->memory == current->memory &&
                     item->futex_wait_address == address);
        if (item->state == PROCESS_BLOCKED && item->futex_wait_active && named &&
            (item->futex_wait_bitset & bitset)) {
            item->futex_wait_active = 0;
            item->futex_wait_address = 0;
            item->futex_wait_key = 0;
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
                (void)process_futex_wake(clear_address, 1, FUTEX_BITSET_MATCH_ANY, 1);
            }
            mark_dead(item);
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
    fpu_init_state(current);
    fpu_restore(current);
    current->user_stack_top = image.user_stack_top;
    current->brk_start = image.brk_start;
    current->brk_end = image.brk_end;
    current->mmap_base = image.mmap_base;
    current->fs_base = 0;
    current->gs_base = 0;
    current->signal_stack_pointer = 0;
    current->signal_stack_size = 0;
    current->signal_stack_flags = SS_DISABLE;
    current->robust_list_head = 0;
    current->robust_list_length = 0;
    cred_apply_exec(&current->cred, credential_source, current->no_new_privs);
    current->dumpable = (current->cred.euid == current->cred.uid &&
                         current->cred.egid == current->cred.gid);
    strncpy(current->name, file->name, sizeof(current->name) - 1);
    set_exe_path(current, file, path);
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
    cpu_current()->address_space = new_cr3;
    wrmsr(IA32_FS_BASE, 0);
    wrmsr(IA32_KERNEL_GS_BASE, 0);
    if (old_memory) memory_unref(old_memory);
    else vmm_destroy_address_space(old_cr3);
    eventfs_emit_process_exec(current->cred.euid, current->pid, current->name);
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
                    mark_dead(item);
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
    if (process_signal_interrupts_wait()) return -EINTR;

    parent->saved_frame = *frame;
    parent->state = PROCESS_BLOCKED;
    parent->wait4_active = 1;
    parent->wait_pid = pid;
    parent->wait_status_user = status_user;
    parent->wait_options = options;
    if (switch_to_next(frame, parent) != 0) go_idle();
    return 0;
}

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
                    if (!(options & WNOWAIT)) mark_dead(item);
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
                    if (!(options & WNOWAIT)) item->stop_reported = 1;
                    return 0;
                }
                if ((options & WCONTINUED) && item->continued_pending) {
                    info.si_signo = SIGCHLD;
                    info.si_pid = (int32_t)item->pid;
                    info.si_code = CLD_CONTINUED;
                    info.si_status = SIGCONT;
                    if (vmm_copy_to_space(parent->cr3, info_user, &info,
                                          sizeof(info)) != 0) return -EFAULT;
                    if (!(options & WNOWAIT)) item->continued_pending = 0;
                    return 0;
                }
            }
            item = item->next;
        } while (item != queue);
    }
    if (!has_child) return -ECHILD;
    if (options & WNOHANG) {
        if (vmm_copy_to_space(parent->cr3, info_user, &info, sizeof(info)) != 0)
            return -EFAULT;
        return 0;
    }
    return -EAGAIN;
}

static void signal_one_process(struct process *target, int signal_number) {
    if (signal_number == 0) return;
    eventfs_emit_process_signal(target->cred.euid, target->pid, signal_number);
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
        target->futex_wait_key = 0;
        target->futex_wait_deadline_ns = 0;
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

static int may_signal(const struct process *target) {
    const struct credentials *sender = &current->cred;
    if (sender->euid == 0) return 1;
    return sender->uid == target->cred.uid || sender->uid == target->cred.suid ||
           sender->euid == target->cred.uid || sender->euid == target->cred.suid;
}

static void record_sender(struct process *target, int signal_number) {
    if (signal_number < 1 || signal_number > TUNIX_NSIG) return;
    target->signal_user_sent |= signal_bit(signal_number);
    target->signal_sender_pid[signal_number - 1] = (uint32_t)current->pid;
    target->signal_sender_uid[signal_number - 1] = current->cred.uid;
}

static int send_signal(int64_t pid, int signal_number, int checked) {
    if (signal_number < 0 || signal_number > TUNIX_NSIG) return -EINVAL;
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

static void read_user_context(struct syscall_frame *frame, const uint8_t *context);

int process_sigreturn(struct syscall_frame *frame) {
    if (!current || !frame || !current->in_signal) return -EINVAL;
    *frame = current->signal_saved_frame;
    if (current->signal_context_address) {
        uint8_t context[SIGNAL_CONTEXT_SIZE];
        if (vmm_copy_from_space(current->cr3, context,
                                current->signal_context_address,
                                SIGNAL_CONTEXT_SIZE) == 0)
            read_user_context(frame, context);
    }
    current->signal_context_address = 0;
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

static int signal_would_act(const struct process *process, int signal_number) {
    if (!process || !signal_number) return 0;
    if (signal_number == SIGKILL || signal_number == SIGSTOP) return 1;
    const struct tunix_sigaction *action = &process->signal_actions[signal_number - 1];
    if (action->handler == SIG_IGN) return 0;
    if (action->handler == SIG_DFL &&
        (signal_number == SIGCHLD || signal_number == SIGCONT)) return 0;
    return 1;
}

int process_signal_interrupts_wait(void) {
    if (!current || current->in_signal) return 0;
    if (!current->group_exit_pending &&
        !signal_would_act(current, next_pending_signal(current))) return 0;
    current->syscall_rewound = 0;
    return 1;
}

static void mcontext_put(uint8_t *context, unsigned slot, uint64_t value) {
    memcpy(context + UCONTEXT_MCONTEXT_OFFSET + slot * 8U, &value, sizeof(value));
}

static uint64_t mcontext_get(const uint8_t *context, unsigned slot) {
    uint64_t value;
    memcpy(&value, context + UCONTEXT_MCONTEXT_OFFSET + slot * 8U, sizeof(value));
    return value;
}

static void fill_user_context(uint8_t *context, const struct syscall_frame *frame,
                              uint64_t blocked) {
    mcontext_put(context, MCONTEXT_R8, frame->r8);
    mcontext_put(context, MCONTEXT_R9, frame->r9);
    mcontext_put(context, MCONTEXT_R10, frame->r10);
    mcontext_put(context, MCONTEXT_R11, frame->r11);
    mcontext_put(context, MCONTEXT_R12, frame->r12);
    mcontext_put(context, MCONTEXT_R13, frame->r13);
    mcontext_put(context, MCONTEXT_R14, frame->r14);
    mcontext_put(context, MCONTEXT_R15, frame->r15);
    mcontext_put(context, MCONTEXT_RDI, frame->rdi);
    mcontext_put(context, MCONTEXT_RSI, frame->rsi);
    mcontext_put(context, MCONTEXT_RBP, frame->rbp);
    mcontext_put(context, MCONTEXT_RBX, frame->rbx);
    mcontext_put(context, MCONTEXT_RDX, frame->rdx);
    mcontext_put(context, MCONTEXT_RAX, frame->rax);
    mcontext_put(context, MCONTEXT_RCX, frame->rcx);
    mcontext_put(context, MCONTEXT_RSP, frame->user_rsp);
    mcontext_put(context, MCONTEXT_RIP, frame->user_rip);
    mcontext_put(context, MCONTEXT_EFLAGS, frame->user_rflags);
    memcpy(context + UCONTEXT_SIGMASK_OFFSET, &blocked, sizeof(blocked));
    uint64_t stack_pointer = current ? current->signal_stack_pointer : 0;
    uint64_t stack_size = current ? current->signal_stack_size : 0;
    uint32_t stack_flags = current ? (uint32_t)current->signal_stack_flags : SS_DISABLE;
    memcpy(context + UCONTEXT_STACK_OFFSET, &stack_pointer, sizeof(stack_pointer));
    memcpy(context + UCONTEXT_STACK_OFFSET + 8, &stack_flags, sizeof(stack_flags));
    memcpy(context + UCONTEXT_STACK_OFFSET + 16, &stack_size, sizeof(stack_size));
}

static void read_user_context(struct syscall_frame *frame, const uint8_t *context) {
    frame->r8 = mcontext_get(context, MCONTEXT_R8);
    frame->r9 = mcontext_get(context, MCONTEXT_R9);
    frame->r10 = mcontext_get(context, MCONTEXT_R10);
    frame->r11 = mcontext_get(context, MCONTEXT_R11);
    frame->r12 = mcontext_get(context, MCONTEXT_R12);
    frame->r13 = mcontext_get(context, MCONTEXT_R13);
    frame->r14 = mcontext_get(context, MCONTEXT_R14);
    frame->r15 = mcontext_get(context, MCONTEXT_R15);
    frame->rdi = mcontext_get(context, MCONTEXT_RDI);
    frame->rsi = mcontext_get(context, MCONTEXT_RSI);
    frame->rbp = mcontext_get(context, MCONTEXT_RBP);
    frame->rbx = mcontext_get(context, MCONTEXT_RBX);
    frame->rdx = mcontext_get(context, MCONTEXT_RDX);
    frame->rax = mcontext_get(context, MCONTEXT_RAX);
    frame->rcx = mcontext_get(context, MCONTEXT_RCX);
    uint64_t rsp = mcontext_get(context, MCONTEXT_RSP);
    uint64_t rip = mcontext_get(context, MCONTEXT_RIP);
    if (rsp && rsp < USER_ADDRESS_LIMIT) frame->user_rsp = rsp;
    if (rip && rip < USER_ADDRESS_LIMIT) frame->user_rip = rip;
    uint64_t flags = mcontext_get(context, MCONTEXT_EFLAGS);
    frame->user_rflags = (flags & ~(uint64_t)0x200D5UL & 0x3F7FD5UL) | 0x202UL;
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

    if (!signal_would_act(current, signal_number)) return;
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
    uint64_t area = stack_top & ~15ULL;
    uint64_t siginfo_address = 0;
    uint64_t context_address = 0;
    if (action->flags & SA_SIGINFO) {
        uint8_t context[SIGNAL_CONTEXT_SIZE];
        memset(context, 0, sizeof(context));
        fill_user_context(context, frame, current->signal_blocked);
        area -= SIGNAL_CONTEXT_SIZE;
        context_address = area;
        if (vmm_copy_to_space(current->cr3, context_address, context, SIGNAL_CONTEXT_SIZE) != 0) {
            process_exit_from_signal(frame, SIGSEGV);
            return;
        }
        int32_t info[SIGNAL_SIGINFO_SIZE / 4];
        memset(info, 0, sizeof(info));
        info[0] = signal_number;
        int from_user = (current->signal_user_sent & bit) != 0;
        info[2] = from_user ? SI_USER : SI_KERNEL;
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
    current->signal_context_address = context_address;
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

void process_set_gs_base(uint64_t value) {
    if (!current) return;
    current->gs_base = value;
    wrmsr(IA32_KERNEL_GS_BASE, value);
}

uint64_t process_get_gs_base(void) {
    return current ? current->gs_base : 0;
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
