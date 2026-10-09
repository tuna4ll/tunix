#include <stddef.h>
#include <stdint.h>
#include "include/build_config.h"
#include "include/cred.h"
#include "include/elf.h"
#include "include/eventfs.h"
#include "include/file.h"
#include "include/memfd.h"
#include "include/gdt.h"
#include "include/heap.h"
#include "include/interrupt.h"
#include "include/defer.h"
#include "include/lock.h"
#include "include/usercopy.h"

static int process_wake_all_locked(const void *channel);
static int signal_would_act(const struct process *process, int signal_number);
#include "include/kstring.h"
#include "include/percpu.h"
#include "include/pmm.h"
#include "include/process.h"
#include "include/workqueue.h"
#include "include/process_arch.h"
#include "include/procfs.h"
#include "include/smp.h"
#include "include/syscall.h"
#include "include/time.h"
#include "include/timer.h"
#include "include/cpufreq.h"
#include "include/thermal.h"
#include "include/tty.h"
#include "include/vt.h"
#include "include/vfs.h"
#include "include/vmm.h"

#define KERNEL_STACK_SIZE (32 * 1024)
#define ECHILD 10
#define EINTR 4
#define EINVAL 22
#define ESRCH 3
#define EDEADLK 35
#define EPERM 1
#define EACCES 13
#define EAGAIN 11
#define EFAULT 14
#define ETIMEDOUT 110
#define SIGSEGV 11
#define FUTEX_OWNER_DIED 0x40000000U
#define FUTEX_TID_MASK 0x3fffffffU
#define FUTEX_WAITERS 0x80000000U
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
static unsigned ready_processes;
static struct process *dead_head;
static uint64_t live_processes;

#define WAIT_BUCKETS 1024U
static struct process *wait_buckets[WAIT_BUCKETS];
static struct process *key_buckets[WAIT_BUCKETS];

static struct process *boot_pid_buckets[64];
static struct process **pid_buckets = boot_pid_buckets;
static uint64_t pid_bucket_count = 64;

static uint64_t earliest_deadline = UINT64_MAX;

static unsigned wait_bucket_of(uint64_t value) {
    return (unsigned)((value * 0x9E3779B97F4A7C15ULL) >> 54) & (WAIT_BUCKETS - 1U);
}

static struct process *rq_root;
static uint64_t rq_weight_total;
static unsigned rq_ordinary;
static struct process *rt_heads[PROCESS_RT_PRIORITY_MAX + 1];
static uint64_t rt_bitmap[2];
static uint32_t rq_seed = 0x9E3779B9U;

static uint64_t rq_sequence;

static int rq_before(const struct process *a, const struct process *b) {
    int64_t delta = (int64_t)(a->virtual_runtime_ns - b->virtual_runtime_ns);
    if (delta) return delta < 0;
    return (int64_t)(a->rq_order - b->rq_order) < 0;
}

static void rq_rotate_up(struct process *node) {
    struct process *parent = node->rq_parent;
    struct process *grand = parent->rq_parent;
    if (parent->rq_left == node) {
        parent->rq_left = node->rq_right;
        if (node->rq_right) node->rq_right->rq_parent = parent;
        node->rq_right = parent;
    } else {
        parent->rq_right = node->rq_left;
        if (node->rq_left) node->rq_left->rq_parent = parent;
        node->rq_left = parent;
    }
    parent->rq_parent = node;
    node->rq_parent = grand;
    if (!grand) rq_root = node;
    else if (grand->rq_left == parent) grand->rq_left = node;
    else grand->rq_right = node;
}

static void rq_insert(struct process *node) {
    rq_seed ^= rq_seed << 13;
    rq_seed ^= rq_seed >> 17;
    rq_seed ^= rq_seed << 5;
    node->rq_priority = rq_seed;
    node->rq_order = ++rq_sequence;
    node->rq_left = node->rq_right = node->rq_parent = NULL;
    struct process **link = &rq_root;
    struct process *parent = NULL;
    while (*link) {
        parent = *link;
        link = rq_before(node, parent) ? &parent->rq_left : &parent->rq_right;
    }
    *link = node;
    node->rq_parent = parent;
    while (node->rq_parent && node->rq_priority < node->rq_parent->rq_priority)
        rq_rotate_up(node);
}

static void rq_remove(struct process *node) {
    while (node->rq_left || node->rq_right) {
        struct process *child = !node->rq_left ? node->rq_right
                              : !node->rq_right ? node->rq_left
                              : node->rq_left->rq_priority < node->rq_right->rq_priority
                                    ? node->rq_left : node->rq_right;
        rq_rotate_up(child);
    }
    struct process *parent = node->rq_parent;
    if (!parent) rq_root = NULL;
    else if (parent->rq_left == node) parent->rq_left = NULL;
    else parent->rq_right = NULL;
    node->rq_parent = NULL;
}

static struct process *rq_first(void) {
    struct process *node = rq_root;
    while (node && node->rq_left) node = node->rq_left;
    return node;
}

static struct process *rq_next(struct process *node) {
    if (node->rq_right) {
        node = node->rq_right;
        while (node->rq_left) node = node->rq_left;
        return node;
    }
    while (node->rq_parent && node->rq_parent->rq_right == node) node = node->rq_parent;
    return node->rq_parent;
}

static void ready_link(struct process *process) {
    if (process->on_ready_list) return;
    process->on_ready_list = 1;
    ready_processes++;
    if (process->rt_priority > 0) {
        int level = process->rt_priority > PROCESS_RT_PRIORITY_MAX ? PROCESS_RT_PRIORITY_MAX
                                                                   : process->rt_priority;
        process->rq_level = level;
        struct process *head = rt_heads[level];
        if (!head) {
            rt_heads[level] = process;
            process->ready_next = process->ready_prev = process;
        } else {
            process->ready_prev = head->ready_prev;
            process->ready_next = head;
            head->ready_prev->ready_next = process;
            head->ready_prev = process;
        }
        rt_bitmap[level / 64] |= 1ULL << (level % 64);
        return;
    }
    process->rq_level = 0;
    process->rq_weight = process_weight(process);
    rq_weight_total += process->rq_weight;
    rq_ordinary++;
    rq_insert(process);
}

static void ready_unlink(struct process *process) {
    if (!process->on_ready_list) return;
    process->on_ready_list = 0;
    ready_processes--;
    int level = process->rq_level;
    if (level > 0) {
        if (process->ready_next == process) {
            rt_heads[level] = NULL;
            rt_bitmap[level / 64] &= ~(1ULL << (level % 64));
        } else {
            process->ready_prev->ready_next = process->ready_next;
            process->ready_next->ready_prev = process->ready_prev;
            if (rt_heads[level] == process) rt_heads[level] = process->ready_next;
        }
        process->ready_next = process->ready_prev = NULL;
        return;
    }
    rq_weight_total -= process->rq_weight;
    rq_ordinary--;
    rq_remove(process);
}

static void wait_link(struct process *process, uint64_t value) {
    if (process->on_wait_list) return;
    struct process **bucket = &wait_buckets[wait_bucket_of(value)];
    process->wait_hash_value = value;
    process->wait_prev = NULL;
    process->wait_next = *bucket;
    if (*bucket) (*bucket)->wait_prev = process;
    *bucket = process;
    process->on_wait_list = 1;
}

static void key_link(struct process *process, uint64_t key) {
    if (process->on_key_list || !key) return;
    struct process **bucket = &key_buckets[wait_bucket_of(key)];
    process->key_hash_value = key;
    process->key_prev = NULL;
    process->key_next = *bucket;
    if (*bucket) (*bucket)->key_prev = process;
    *bucket = process;
    process->on_key_list = 1;
}

static void waits_unlink(struct process *process) {
    if (process->on_wait_list) {
        struct process **bucket = &wait_buckets[wait_bucket_of(process->wait_hash_value)];
        if (process->wait_prev) process->wait_prev->wait_next = process->wait_next;
        else *bucket = process->wait_next;
        if (process->wait_next) process->wait_next->wait_prev = process->wait_prev;
        process->wait_next = process->wait_prev = NULL;
        process->on_wait_list = 0;
    }
    if (process->on_key_list) {
        struct process **bucket = &key_buckets[wait_bucket_of(process->key_hash_value)];
        if (process->key_prev) process->key_prev->key_next = process->key_next;
        else *bucket = process->key_next;
        if (process->key_next) process->key_next->key_prev = process->key_prev;
        process->key_next = process->key_prev = NULL;
        process->on_key_list = 0;
    }
}

static void set_process_state(struct process *process, int state) {
    int was_listed_ready = process->on_ready_list;
    if (process->state == PROCESS_BLOCKED && state != PROCESS_BLOCKED) waits_unlink(process);
    if (state == PROCESS_READY) ready_link(process);
    else if (was_listed_ready) ready_unlink(process);
    process->state = state;
}

static void note_deadline(uint64_t deadline) {
    if (deadline && deadline < earliest_deadline) earliest_deadline = deadline;
    timer_note_deadline(deadline);
}

static void pid_link(struct process *process) {
    if (live_processes + 1 > pid_bucket_count * 2) {
        uint64_t count = pid_bucket_count * 2;
        struct process **buckets = (struct process **)kmalloc(count * sizeof(*buckets));
        if (buckets) {
            memset(buckets, 0, count * sizeof(*buckets));
            for (uint64_t index = 0; index < pid_bucket_count; index++) {
                struct process *item = pid_buckets[index];
                while (item) {
                    struct process *next = item->pid_next;
                    struct process **slot = &buckets[item->pid & (count - 1)];
                    item->pid_next = *slot;
                    *slot = item;
                    item = next;
                }
            }
            if (pid_buckets != boot_pid_buckets) kfree(pid_buckets);
            pid_buckets = buckets;
            pid_bucket_count = count;
        }
    }
    struct process **slot = &pid_buckets[process->pid & (pid_bucket_count - 1)];
    process->pid_next = *slot;
    *slot = process;
    live_processes++;
}

static void pid_unlink(struct process *process) {
    struct process **slot = &pid_buckets[process->pid & (pid_bucket_count - 1)];
    while (*slot && *slot != process) slot = &(*slot)->pid_next;
    if (*slot) {
        *slot = process->pid_next;
        live_processes--;
    }
    process->pid_next = NULL;
}
static int reap_pending;
static int zombie_memory_pending;

#define current (cpu_current()->current)

static struct lock sched_lock = LOCK_INITIALIZER("scheduler", LOCK_RANK_SCHED);

static void sched_guard_release(int *unused) {
    (void)unused;
    lock_release(&sched_lock);
}

#define SCHED_LOCKED \
    __attribute__((cleanup(sched_guard_release))) int sched_guard = (lock_acquire(&sched_lock), 0)

static volatile uint64_t wake_sequence;
static volatile int io_recheck_pending;

static void wake_bump(void) {
    __atomic_add_fetch(&wake_sequence, 1, __ATOMIC_SEQ_CST);
}

static int wake_missed(void) {
    return current &&
           __atomic_load_n(&wake_sequence, __ATOMIC_SEQ_CST) != current->wake_snapshot;
}

void process_note_syscall_entry(void) {
    if (current) current->wake_snapshot = __atomic_load_n(&wake_sequence, __ATOMIC_SEQ_CST);
}

void process_table_lock(void) {
    lock_acquire(&sched_lock);
}

void process_table_unlock(void) {
    lock_release(&sched_lock);
}

static void signal_one_process(struct process *target, int signal_number);
static int send_signal(int64_t pid, int signal_number, int checked);

static struct process_memory *memory_create(uint64_t cr3, uint64_t brk_start,
                                            uint64_t brk_end, uint64_t mmap_base) {
    struct process_memory *memory = (struct process_memory *)kmalloc(sizeof(*memory));
    if (!memory) return NULL;
    memset(memory, 0, sizeof(*memory));
    mutex_init(&memory->lock, "address space", LOCK_RANK_MEMORY);
    memory->cr3 = cr3;
    memory->refs = 1;
    memory->brk_start = brk_start;
    memory->brk_end = brk_end;
    memory->mmap_base = mmap_base;
    return memory;
}

static void memory_ref(struct process_memory *memory) {
    if (memory) __atomic_add_fetch(&memory->refs, 1, __ATOMIC_RELAXED);
}

static void areas_free(struct process_memory *memory);
static int areas_copy(struct process_memory *destination,
                      const struct process_memory *source);

static void memory_unref(struct process_memory *memory) {
    if (!memory || __atomic_load_n(&memory->refs, __ATOMIC_RELAXED) == 0) return;
    if (__atomic_sub_fetch(&memory->refs, 1, __ATOMIC_ACQ_REL) != 0) return;
    areas_free(memory);
    if (memory->cr3) vmm_destroy_address_space(memory->cr3);
    kfree(memory);
}

void process_memory_enter(void) {
    struct process *self = current;
    if (self && self->memory) mutex_lock(&self->memory->lock);
}

void process_memory_leave(void) {
    struct process *self = current;
    if (self && self->memory) mutex_unlock(&self->memory->lock);
}

struct process_memory *process_memory_get(struct process *process) {
    if (!process) return NULL;
    SCHED_LOCKED;
    struct process_memory *memory = process->memory;
    memory_ref(memory);
    return memory;
}

void process_memory_put(struct process_memory *memory) {
    memory_unref(memory);
}

static void memory_guard_release(int *unused) {
    (void)unused;
    process_memory_leave();
}

#define MEMORY_LOCKED \
    __attribute__((cleanup(memory_guard_release))) int memory_guard = (process_memory_enter(), 0)

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

uint64_t process_stack_floor(const struct process *process) {
    uint64_t reserve = process ? process->rlimits[PROCESS_RLIMIT_STACK].soft : 0;
    if (reserve < USER_STACK_RESERVE_MIN) reserve = USER_STACK_RESERVE_MIN;
    if (reserve > USER_STACK_RESERVE_MAX) reserve = USER_STACK_RESERVE_MAX;
    return (USER_STACK_TOP - reserve) & ~4095ULL;
}

uint64_t process_arg_limit(const struct process *process) {
    uint64_t limit = process ? process->rlimits[PROCESS_RLIMIT_STACK].soft / 4 : 0;
    if (limit > 6ULL * 1024 * 1024) limit = 6ULL * 1024 * 1024;
    if (limit < 128ULL * 1024) limit = 128ULL * 1024;
    return limit;
}

static uint64_t signal_bit(int signal_number) {
    if (signal_number < 1 || signal_number > TUNIX_NSIG) return 0;
    return 1ULL << (signal_number - 1);
}

void process_init(void) {
    queue = NULL;
    current = NULL;
}

static void sibling_unlink(struct process *process) {
    struct process *parent = process->linked_parent;
    if (!parent) return;
    if (process->sibling_prev) process->sibling_prev->sibling_next = process->sibling_next;
    else parent->children = process->sibling_next;
    if (process->sibling_next) process->sibling_next->sibling_prev = process->sibling_prev;
    else parent->children_tail = process->sibling_prev;
    process->sibling_next = process->sibling_prev = NULL;
    process->linked_parent = NULL;
}

static void sibling_attach(struct process *parent, struct process *process, int front) {
    process->linked_parent = parent;
    if (front) {
        process->sibling_prev = NULL;
        process->sibling_next = parent->children;
        if (parent->children) parent->children->sibling_prev = process;
        else parent->children_tail = process;
        parent->children = process;
    } else {
        process->sibling_next = NULL;
        process->sibling_prev = parent->children_tail;
        if (parent->children_tail) parent->children_tail->sibling_next = process;
        else parent->children = process;
        parent->children_tail = process;
    }
}

static void sibling_link(struct process *process) {
    sibling_unlink(process);
    struct process *parent = process->ppid ? process_find(process->ppid) : NULL;
    if (!parent || parent == process) return;
    sibling_attach(parent, process, process->state == PROCESS_ZOMBIE);
}

static void sibling_to_front(struct process *process) {
    struct process *parent = process->linked_parent;
    if (!parent || parent->children == process) return;
    sibling_unlink(process);
    sibling_attach(parent, process, 1);
}

static void enqueue(struct process *process) {
    pid_link(process);
    if (!queue) {
        queue = process;
        process->next = process->prev = process;
        return;
    }
    struct process *tail = queue->prev;
    tail->next = process;
    process->prev = tail;
    process->next = queue;
    queue->prev = process;
    sibling_link(process);
}

static void dequeue(struct process *process) {
    sibling_unlink(process);
    for (struct process *child = process->children; child;) {
        struct process *next = child->sibling_next;
        child->sibling_next = child->sibling_prev = NULL;
        child->linked_parent = NULL;
        child = next;
    }
    process->children = NULL;
    process->children_tail = NULL;
    pid_unlink(process);
    if (process->next == process) {
        queue = NULL;
    } else {
        process->prev->next = process->next;
        process->next->prev = process->prev;
        if (queue == process) queue = process->next;
    }
    process->next = process->prev = NULL;
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
    SCHED_LOCKED;
    process_dump_wakes();
    kprintf("PROCESSES: ready %u ticks %u\n", ready_processes, (unsigned)timer_ticks());
    for (unsigned index = 0; index < SMP_MAX_CPUS; index++) {
        struct cpu *cpu = percpu_slot(index);
        if (!cpu || !cpu->online) continue;
        struct process *running = cpu_running(cpu);
        kprintf("  cpu %u runs %d%s\n", index, running ? (int)running->pid : -1,
                cpu->switching ? " switching" : "");
    }
    if (!queue) return;
    struct process *item = queue;
    do {
        if (item->state == PROCESS_DEAD) { item = item->next; continue; }
        uint64_t rip = item == current ? SYSCALL_IP(&item->saved_frame)
                                       : SYSCALL_IP(&item->saved_frame);
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
        if (item->kernel_waiting) kprintf(" in-kernel");
        if (item->waiting_for) {
            struct process *owner = item->waiting_for->owner;
            kprintf(" mutex=%s owner=%d", item->waiting_for->name,
                    owner && !((uintptr_t)owner & 1U) ? (int)owner->pid : -1);
        }
        if (item->kernel_suspended) kprintf(" suspended");
        if (item->on_ready_list) kprintf(" listed");
        if (item->affinity_set) {
            kprintf(" cpus=");
            for (unsigned index = 0; index < smp_cpu_count(); index++)
                if (cpu_mask_test(&item->affinity, index)) kprintf("%u,", index);
        }
        if (item->on_cpu) kprintf(" on-cpu");
        for (uint32_t index = 0; index < item->held_mutex_count && index < PROCESS_HELD_MUTEXES;
             index++)
            kprintf(" holds=%s", item->held_mutexes[index]->name);
        if (item->syscall_rewound) kprintf(" rewound");
        kprintf(" at %s+%p" "\n", object, (void *)offset);
        item = item->next;
    } while (item != queue);
}

struct process *process_find(uint64_t pid) {
    if (!queue || pid == 0) return NULL;
    for (struct process *item = pid_buckets[pid & (pid_bucket_count - 1)]; item;
         item = item->pid_next)
        if (item->pid == pid && item->state != PROCESS_DEAD) return item;
    return NULL;
}

int process_pidfd_target(uint64_t pid, uint64_t *start_ns) {
    SCHED_LOCKED;
    struct process *found = process_find(pid);
    if (!found) return -ESRCH;
    if (found->is_thread) return -EINVAL;
    *start_ns = found->start_time_ns;
    return 0;
}

int process_pidfd_exited(uint64_t pid, uint64_t start_ns) {
    SCHED_LOCKED;
    struct process *found = process_find(pid);
    return !found || found->start_time_ns != start_ns || found->state == PROCESS_ZOMBIE;
}

struct process *process_get(uint64_t pid) {
    SCHED_LOCKED;
    struct process *found = process_find(pid);
    if (found) found->refs++;
    return found;
}

static void free_process_struct(struct process *process);

void process_put(struct process *process) {
    if (!process) return;
    int release = 0;
    {
        SCHED_LOCKED;
        if (process->refs) process->refs--;
        release = !process->refs && process->struct_orphaned;
    }
    if (release) free_process_struct(process);
}

int process_exists(uint64_t pid) {
    SCHED_LOCKED;
    return process_find(pid) != NULL;
}

static uint64_t fd_limit(const struct process *process) {
    uint64_t limit = process->rlimits[PROCESS_RLIMIT_NOFILE].soft;
    return limit > PROCESS_NR_OPEN ? PROCESS_NR_OPEN : limit;
}

static int file_table_grow(struct file_table *table, int wanted) {
    if (wanted <= table->capacity) return 0;
    int capacity = table->capacity ? table->capacity : 64;
    while (capacity < wanted) capacity *= 2;
    struct file **fds = (struct file **)kmalloc((size_t)capacity * sizeof(*fds));
    uint8_t *flags = (uint8_t *)kmalloc((size_t)capacity);
    if (!fds || !flags) {
        kfree(fds);
        kfree(flags);
        return -1;
    }
    memset(fds, 0, (size_t)capacity * sizeof(*fds));
    memset(flags, 0, (size_t)capacity);
    if (table->capacity) {
        memcpy(fds, table->fds, (size_t)table->capacity * sizeof(*fds));
        memcpy(flags, table->fd_flags, (size_t)table->capacity);
    }
    kfree(table->fds);
    kfree(table->fd_flags);
    table->fds = fds;
    table->fd_flags = flags;
    table->capacity = capacity;
    return 0;
}

struct file *file_table_get(struct file_table *table, int fd) {
    if (!table || fd < 0) return NULL;
    lock_acquire(&table->lock);
    struct file *file = fd < table->capacity ? table->fds[fd] : NULL;
    if (file) file_ref(file);
    lock_release(&table->lock);
    return file;
}

int file_table_find(struct file_table *table, const struct file *file) {
    if (!table || !file) return -1;
    lock_acquire(&table->lock);
    int found = -1;
    for (int fd = 0; fd < table->capacity; fd++) {
        if (table->fds[fd] != file) continue;
        found = fd;
        break;
    }
    lock_release(&table->lock);
    return found;
}

struct file *process_file_get(struct process *process, int fd) {
    return process ? file_table_get(process->files, fd) : NULL;
}

int process_reserve_fd(struct process *process, int fd) {
    if (!process || !process->files || fd < 0 || (uint64_t)fd >= fd_limit(process)) return -1;
    lock_acquire(&process->files->lock);
    int status = file_table_grow(process->files, fd + 1);
    lock_release(&process->files->lock);
    return status;
}

int process_install_file_flags(struct process *process, struct file *file,
                               int minimum_fd, uint8_t flags) {
    if (!process || !process->files || !file) return -1;
    if (minimum_fd < 0) minimum_fd = 0;
    struct file_table *table = process->files;
    uint64_t limit = fd_limit(process);
    int installed = -1;
    lock_acquire(&table->lock);
    for (uint64_t fd = (uint64_t)minimum_fd; fd < limit; fd++) {
        if (fd >= (uint64_t)table->capacity && file_table_grow(table, (int)fd + 1) != 0)
            break;
        if (!table->fds[fd]) {
            table->fds[fd] = file;
            table->fd_flags[fd] = flags & PROCESS_FD_CLOEXEC;
            installed = (int)fd;
            break;
        }
    }
    lock_release(&table->lock);
    return installed;
}

int process_install_file_at(struct process *process, struct file *file, int fd,
                            uint8_t flags, struct file **replaced) {
    if (replaced) *replaced = NULL;
    if (!process || !process->files || !file || fd < 0 || (uint64_t)fd >= fd_limit(process))
        return -1;
    struct file_table *table = process->files;
    lock_acquire(&table->lock);
    int status = file_table_grow(table, fd + 1);
    if (status == 0) {
        if (replaced) *replaced = table->fds[fd];
        table->fds[fd] = file;
        table->fd_flags[fd] = flags & PROCESS_FD_CLOEXEC;
    }
    lock_release(&table->lock);
    return status;
}

static const struct process_rlimit default_rlimits[PROCESS_RLIMITS] = {
    [0] = {PROCESS_RLIM_INFINITY, PROCESS_RLIM_INFINITY},
    [1] = {PROCESS_RLIM_INFINITY, PROCESS_RLIM_INFINITY},
    [2] = {PROCESS_RLIM_INFINITY, PROCESS_RLIM_INFINITY},
    [PROCESS_RLIMIT_STACK] = {8ULL * 1024 * 1024, PROCESS_RLIM_INFINITY},
    [4] = {0, PROCESS_RLIM_INFINITY},
    [5] = {PROCESS_RLIM_INFINITY, PROCESS_RLIM_INFINITY},
    [6] = {PROCESS_RLIM_INFINITY, PROCESS_RLIM_INFINITY},
    [PROCESS_RLIMIT_NOFILE] = {1024, PROCESS_NR_OPEN},
    [8] = {8ULL * 1024 * 1024, 8ULL * 1024 * 1024},
    [9] = {PROCESS_RLIM_INFINITY, PROCESS_RLIM_INFINITY},
    [10] = {PROCESS_RLIM_INFINITY, PROCESS_RLIM_INFINITY},
    [11] = {PROCESS_RLIM_INFINITY, PROCESS_RLIM_INFINITY},
    [12] = {819200, 819200},
    [13] = {0, 0},
    [14] = {0, 0},
    [15] = {PROCESS_RLIM_INFINITY, PROCESS_RLIM_INFINITY},
};

int process_set_rlimit(struct process *process, unsigned resource,
                       const struct process_rlimit *value) {
    if (!process || resource >= PROCESS_RLIMITS || !value || value->soft > value->hard) return -1;
    if (resource == PROCESS_RLIMIT_NOFILE && value->hard > PROCESS_NR_OPEN) return -1;
    SCHED_LOCKED;
    struct process *item = queue;
    if (!item) return -1;
    do {
        if (item->tgid == process->tgid) item->rlimits[resource] = *value;
        item = item->next;
    } while (item != queue);
    return 0;
}

int process_install_file(struct process *process, struct file *file, int minimum_fd) {
    return process_install_file_flags(process, file, minimum_fd, 0);
}

uint8_t process_get_fd_flags(const struct process *process, int fd) {
    if (!process || !process->files || fd < 0) return 0;
    struct file_table *table = process->files;
    lock_acquire(&table->lock);
    uint8_t flags = fd < table->capacity && table->fds[fd] ? table->fd_flags[fd] : 0;
    lock_release(&table->lock);
    return flags;
}

int process_set_fd_flags(struct process *process, int fd, uint8_t flags) {
    if (!process || !process->files || fd < 0) return -1;
    struct file_table *table = process->files;
    int status = -1;
    lock_acquire(&table->lock);
    if (fd < table->capacity && table->fds[fd]) {
        table->fd_flags[fd] = flags & PROCESS_FD_CLOEXEC;
        status = 0;
    }
    lock_release(&table->lock);
    return status;
}

int process_close_fd(struct process *process, int fd) {
    if (!process || !process->files || fd < 0) return -1;
    struct file_table *table = process->files;
    lock_acquire(&table->lock);
    struct file *file = fd < table->capacity ? table->fds[fd] : NULL;
    if (file) {
        table->fds[fd] = NULL;
        table->fd_flags[fd] = 0;
    }
    lock_release(&table->lock);
    if (!file) return -1;
    file_unref(file);
    return 0;
}

static struct file_table *file_table_create(void) {
    struct file_table *table = (struct file_table *)kmalloc(sizeof(*table));
    if (!table) return NULL;
    memset(table, 0, sizeof(*table));
    lock_init(&table->lock, "file table", LOCK_RANK_FILES);
    table->refs = 1;
    if (file_table_grow(table, 64) != 0) {
        kfree(table);
        return NULL;
    }
    return table;
}

static void file_table_free(struct file_table *table) {
    kfree(table->fds);
    kfree(table->fd_flags);
    kfree(table);
}

static struct file_table *file_table_clone(struct file_table *source) {
    struct file_table *table = file_table_create();
    if (!table || !source) return table;
    lock_acquire(&source->lock);
    if (file_table_grow(table, source->capacity) != 0) {
        lock_release(&source->lock);
        file_table_free(table);
        return NULL;
    }
    for (int fd = 0; fd < source->capacity; fd++) {
        if (source->fds[fd]) {
            table->fds[fd] = source->fds[fd];
            table->fd_flags[fd] = source->fd_flags[fd];
            file_ref(table->fds[fd]);
        }
    }
    lock_release(&source->lock);
    return table;
}

void file_table_ref(struct file_table *table) {
    if (table) __atomic_add_fetch(&table->refs, 1, __ATOMIC_RELAXED);
}

static void file_table_destroy(struct file_table *table) {
    for (int fd = 0; fd < table->capacity; fd++) {
        struct file *file = table->fds[fd];
        if (!file) continue;
        table->fds[fd] = NULL;
        table->fd_flags[fd] = 0;
        file_unref(file);
    }
    file_table_free(table);
}

void file_table_unref(struct file_table *table) {
    if (!table || __atomic_sub_fetch(&table->refs, 1, __ATOMIC_ACQ_REL) > 0) return;
    file_table_destroy(table);
}

static struct file_table *dead_tables;

static void reap_dead_tables(void *unused) {
    (void)unused;
    struct file_table *table = __atomic_exchange_n(&dead_tables, NULL, __ATOMIC_ACQ_REL);
    while (table) {
        struct file_table *next = table->dead_next;
        file_table_destroy(table);
        table = next;
    }
}

static struct work dead_table_work = WORK_INITIALIZER(reap_dead_tables, NULL);

static void file_table_unref_deferred(struct file_table *table) {
    if (!table || __atomic_sub_fetch(&table->refs, 1, __ATOMIC_ACQ_REL) > 0) return;
    struct file_table *head = __atomic_load_n(&dead_tables, __ATOMIC_RELAXED);
    do {
        table->dead_next = head;
    } while (!__atomic_compare_exchange_n(&dead_tables, &head, table, 1, __ATOMIC_RELEASE,
                                          __ATOMIC_RELAXED));
    work_queue(&dead_table_work);
}

void file_table_close_on_exec(struct file_table *table) {
    if (!table) return;
    for (;;) {
        struct file *closing = NULL;
        lock_acquire(&table->lock);
        for (int fd = 0; fd < table->capacity; fd++) {
            if (table->fds[fd] && (table->fd_flags[fd] & PROCESS_FD_CLOEXEC)) {
                closing = table->fds[fd];
                table->fds[fd] = NULL;
                table->fd_flags[fd] = 0;
                break;
            }
        }
        lock_release(&table->lock);
        if (!closing) return;
        file_unref(closing);
    }
}

static void process_release_files(struct process *process) {
    if (!process || !process->files) return;
    struct file_table *table;
    {
        SCHED_LOCKED;
        table = process->files;
        process->files = NULL;
    }
    file_table_unref(table);
}

struct file_table *process_files_get(struct process *process) {
    if (!process) return NULL;
    SCHED_LOCKED;
    struct file_table *table = process->files;
    file_table_ref(table);
    return table;
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

static void free_process_struct(struct process *process);

static void destroy_process_resources(struct process *process) {
    if (!process) return;
    uint64_t pid = process->pid;
#if !TUNIX_DEBUG_LOGS
    (void)pid;
#endif
    procfs_unregister_process(process->pid);
    vfs_node_unref(process->cwd);
    process->cwd = NULL;
    vfs_node_unref(process->root);
    process->root = NULL;
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
    int keep = 0;
    {
        SCHED_LOCKED;
        if (process->refs) {
            process->struct_orphaned = 1;
            keep = 1;
        }
    }
    if (!keep) free_process_struct(process);
    KDEBUG("process: reaped pid=%u\n", (unsigned)pid);
}

static struct process *zombie_head;

static struct process_memory *take_zombie_memory(void) {
    struct process *list = zombie_head;
    struct process *kept = NULL;
    struct process_memory *released = NULL;
    zombie_head = NULL;
    while (list) {
        struct process *item = list;
        list = item->zombie_next;
        if ((item == current || __atomic_load_n(&item->on_cpu, __ATOMIC_ACQUIRE)) &&
            item->memory) {
            item->zombie_next = kept;
            kept = item;
            continue;
        }
        item->zombie_next = NULL;
        item->on_zombie_list = 0;
        if (item->state == PROCESS_ZOMBIE && item->memory) {
            item->memory->release_next = released;
            released = item->memory;
            item->memory = NULL;
            item->cr3 = 0;
        }
    }
    zombie_head = kept;
    zombie_memory_pending = kept != NULL;
    return released;
}

static void zombie_forget(struct process *process) {
    if (!process->on_zombie_list) return;
    for (struct process **link = &zombie_head; *link; link = &(*link)->zombie_next) {
        if (*link == process) {
            *link = process->zombie_next;
            break;
        }
    }
    process->zombie_next = NULL;
    process->on_zombie_list = 0;
}

void process_reap_deferred(void) {
    if (!__atomic_load_n(&zombie_memory_pending, __ATOMIC_RELAXED) &&
        !__atomic_load_n(&reap_pending, __ATOMIC_RELAXED)) return;
    struct process_memory *memories = NULL;
    struct process *victims = NULL;
    {
        SCHED_LOCKED;
        if (zombie_memory_pending) memories = take_zombie_memory();
        if (reap_pending) {
            struct process *list = dead_head;
            struct process *kept = NULL;
            dead_head = NULL;
            while (list) {
                struct process *victim = list;
                list = victim->dead_next;
                victim->dead_next = NULL;
                if (victim == current || __atomic_load_n(&victim->on_cpu, __ATOMIC_ACQUIRE)) {
                    victim->dead_next = kept;
                    kept = victim;
                    continue;
                }
                victim->on_dead_list = 0;
                zombie_forget(victim);
                dequeue(victim);
                ready_unlink(victim);
                waits_unlink(victim);
                victim->dead_next = victims;
                victims = victim;
            }
            dead_head = kept;
            reap_pending = kept != NULL;
        }
    }
    while (memories) {
        struct process_memory *next = memories->release_next;
        memory_unref(memories);
        memories = next;
    }
    while (victims) {
        struct process *next = victims->dead_next;
        victims->dead_next = NULL;
        destroy_process_resources(victims);
        victims = next;
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

static char *copy_text(const char *text) {
    size_t length = strlen(text ? text : "");
    char *copy = (char *)defer_alloc(length + 1);
    if (copy) memcpy(copy, text ? text : "", length + 1);
    return copy;
}

static void set_exe_path(struct process *process, struct vfs_node *file,
                         const char *path) {
    VFS_PATH_SCOPED resolved = vfs_path_buffer();
    if (resolved && file && vfs_node_path(file, resolved, VFS_PATH_MAX) == 0) path = resolved;
    char *copy = copy_text(path);
    if (!copy) return;
    char *previous = __atomic_exchange_n(&process->exe_path, copy, __ATOMIC_ACQ_REL);
    defer_free(previous);
}

static void free_process_struct(struct process *process) {
    cgroup_drop(process);
    cred_groups_release(&process->cred);
    kfree(process->io_watch_fd);
    kfree(process->io_watch_events);
    defer_free(process->exe_path);
    kfree(process);
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
    memcpy(process->rlimits, default_rlimits, sizeof(process->rlimits));
    process->pid = __atomic_fetch_add(&next_pid, 1, __ATOMIC_RELAXED);
    process->tgid = process->pid;
    process->ppid = 0;
    process->pgid = process->pid;
    process->sid = process->pid;
    process->umask = 022;
    process->signal_stack_flags = SS_DISABLE;
    process->dumpable = 1;
    process->timerslack_ns = DEFAULT_TIMERSLACK_NS;
    process->files = file_table_create();
    if (!process->files) {
        kprintf("process: descriptor-table allocation failed for %s\n", path);
        free_process_struct(process);
        return NULL;
    }
    process->cwd = vfs_root;
    vfs_node_ref(process->cwd);
    process->root = NULL;
    strncpy(process->name, file->name, sizeof(process->name) - 1);
    set_exe_path(process, file, path);
    process->cr3 = vmm_create_address_space();
    if (!process->cr3) {
        kprintf("process: address-space creation failed for %s\n", path);
        process_release_files(process);
        free_process_struct(process);
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
    if (elf_load_process(process, file, argv, envp) != 0) {
        kprintf("process: invalid ELF64: %s\n", path);
        vmm_destroy_address_space(process->cr3);
        process_release_files(process);
        free_process_struct(process);
        return NULL;
    }
    process->memory = memory_create(process->cr3, process->brk_start,
                                    process->brk_end, process->mmap_base);
    if (!process->memory) {
        vmm_destroy_address_space(process->cr3);
        process_release_files(process);
        free_process_struct(process);
        return NULL;
    }

    fpu_init_state(process);
    if (allocate_kernel_stack(process) != 0) {
        kprintf("process: kernel stack allocation failed for %s\n", path);
        memory_unref(process->memory);
        process_release_files(process);
        free_process_struct(process);
        return NULL;
    }
    arch_frame_enter_user(&process->saved_frame, process->entry, process->user_stack_top);
    install_console(process);

    {
        SCHED_LOCKED;
        enqueue(process);
        ready_link(process);
    }
    procfs_register_process(process);
    eventfs_emit_process_exec(process->cred.euid, process->pid, process->name);
    if (process->pid == 1) tty_set_foreground_pgid(vt_tty(1U), (int)process->pgid);
    KDEBUG("process: pid=%u path=%s entry=%p cr3=%p\n",
            (unsigned)process->pid, path, (void *)process->entry, (void *)process->cr3);
    return process;
}

struct process *process_create_kthread(const char *name, void (*body)(void *),
                                       void *argument) {
    struct process *process = (struct process *)kmalloc(sizeof(*process));
    if (!process) return NULL;
    memset(process, 0, sizeof(*process));
    memcpy(process->rlimits, default_rlimits, sizeof(process->rlimits));
    process->pid = __atomic_fetch_add(&next_pid, 1, __ATOMIC_RELAXED);
    process->tgid = process->pid;
    process->pgid = 0;
    process->sid = 0;
    process->is_kthread = 1;
    process->signal_stack_flags = SS_DISABLE;
    process->timerslack_ns = DEFAULT_TIMERSLACK_NS;
    process->cwd = vfs_root;
    vfs_node_ref(process->cwd);
    strncpy(process->name, name, sizeof(process->name) - 1);
    process->cr3 = vmm_kernel_cr3();
    process->start_time_ns = time_uptime_ns();
    process->signal_blocked = ~0ULL;
    fpu_init_state(process);
    if (allocate_kernel_stack(process) != 0) {
        vfs_node_unref(process->cwd);
        free_process_struct(process);
        return NULL;
    }
    process->kernel_sp = arch_context_init(process->kernel_stack_top, 0, arch_kthread_entry,
                                           (uint64_t)body, (uint64_t)argument);
    process->kernel_suspended = 1;
    process->sched_depth = 1;
    {
        SCHED_LOCKED;
        enqueue(process);
        set_process_state(process, PROCESS_READY);
    }
    procfs_register_process(process);
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
    SCHED_LOCKED;
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
    return !process->affinity_set || cpu_mask_test(&process->affinity, cpu_current()->index);
}

static int runnable(const struct process *process) {
    return process && process->state == PROCESS_READY && allowed_on_this_cpu(process) &&
           (!__atomic_load_n(&process->on_cpu, __ATOMIC_ACQUIRE) || process == current ||
            process == cpu_current()->switch_owner);
}

static uint64_t minimum_virtual_runtime;

static void place_waking_task(struct process *process) {
    if (!process || process->rt_priority || process->on_ready_list) return;
    uint64_t credit = SCHED_TARGET_LATENCY_NS / 2;
    uint64_t floor = minimum_virtual_runtime > credit ? minimum_virtual_runtime - credit : 0;
    if (process->virtual_runtime_ns < floor) process->virtual_runtime_ns = floor;
}

static void mark_dead(struct process *process) {
    if (!process) return;
    set_process_state(process, PROCESS_DEAD);
    if (!process->on_dead_list) {
        process->on_dead_list = 1;
        process->dead_next = dead_head;
        dead_head = process;
    }
    reap_pending = 1;
}

static int wakeup_preempts(const struct process *woken, const struct process *running) {
    if (!running || running->state != PROCESS_RUNNING) return 0;
    if (woken->rt_priority != running->rt_priority)
        return woken->rt_priority > running->rt_priority;
    if (woken->rt_priority) return 0;
    return (int64_t)(woken->virtual_runtime_ns + SCHED_WAKEUP_GRANULARITY_NS -
                     running->virtual_runtime_ns) < 0;
}

static int busier_victim(const struct process *candidate, const struct process *chosen) {
    if (!chosen) return 1;
    if (candidate->rt_priority != chosen->rt_priority)
        return candidate->rt_priority < chosen->rt_priority;
    return (int64_t)(candidate->virtual_runtime_ns - chosen->virtual_runtime_ns) > 0;
}

static void kick_idle_for(const struct process *process) {
    unsigned cpus = smp_cpu_count();
    unsigned victim = SMP_MAX_CPUS;
    const struct process *victim_running = NULL;
    for (unsigned index = 0; index < cpus && index < SMP_MAX_CPUS; index++) {
        struct cpu *cpu = percpu_slot(index);
        if (!cpu || !cpu->online || cpu == cpu_current()) continue;
        if (process->affinity_set && !cpu_mask_test(&process->affinity, index)) continue;
        struct process *running = cpu_running(cpu);
        if (!running) {
            smp_send_reschedule_to(index);
            return;
        }
        if (wakeup_preempts(process, running) && busier_victim(running, victim_running)) {
            victim = index;
            victim_running = running;
        }
    }
    if (victim < SMP_MAX_CPUS) smp_send_reschedule_to(victim);
}

static void wake_to_ready(struct process *process) {
    if (!process) return;
    place_waking_task(process);
    set_process_state(process, PROCESS_READY);
    kick_idle_for(process);
}

static void signal_one_process(struct process *target, int signal_number);

static void wake_expired_timers(uint64_t now) {
    if (!queue || now < earliest_deadline) return;
    earliest_deadline = UINT64_MAX;

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
        if (item->state != PROCESS_DEAD) note_deadline(item->itimer_real_deadline_ns);
        if (item->state == PROCESS_BLOCKED && item->futex_wait_active &&
            item->futex_wait_deadline_ns != UINT64_MAX &&
            now >= item->futex_wait_deadline_ns) {
            item->futex_wait_active = 0;
            item->futex_wait_address = 0;
            item->futex_wait_key = 0;
            item->futex_wait_deadline_ns = 0;
            if (!item->syscall_rewound)
                SYSCALL_RET(&item->saved_frame) = (uint64_t)-(int64_t)ETIMEDOUT;
            wake_to_ready(item);
        } else if (item->state == PROCESS_BLOCKED && item->futex_wait_active &&
                   item->futex_wait_deadline_ns != UINT64_MAX) {
            note_deadline(item->futex_wait_deadline_ns);
        }
        if (item->state == PROCESS_BLOCKED && item->kernel_waiting &&
            item->kernel_wait_deadline_ns) {
            if (now >= item->kernel_wait_deadline_ns) wake_to_ready(item);
            else note_deadline(item->kernel_wait_deadline_ns);
        }
        item = item->next;
    } while (item != queue);
}

static struct process *first_allowed_rt(int above, const struct process *skip) {
    for (int word = 1; word >= 0; word--) {
        uint64_t bits = rt_bitmap[word];
        while (bits) {
            int level = word * 64 + 63 - __builtin_clzll(bits);
            bits &= ~(1ULL << (level % 64));
            if (level <= above) return NULL;
            struct process *head = rt_heads[level];
            struct process *walk = head;
            do {
                if (walk != skip && runnable(walk)) return walk;
                walk = walk->ready_next;
            } while (walk != head);
        }
    }
    return NULL;
}

static struct process *first_allowed_ordinary(const struct process *skip) {
    for (struct process *walk = rq_first(); walk; walk = rq_next(walk))
        if (walk != skip && runnable(walk)) return walk;
    return NULL;
}

static int higher_priority_waiting(const struct process *than) {
    if (!than || !ready_processes) return 0;
    return first_allowed_rt(than->rt_priority, than) != NULL;
}

static int ordinary_should_preempt(const struct process *running) {
    if (!running || running->rt_priority || !ready_processes) return 0;
    struct process *walk = first_allowed_ordinary(running);
    return walk && (int64_t)(walk->virtual_runtime_ns + SCHED_WAKEUP_GRANULARITY_NS -
                             running->virtual_runtime_ns) < 0;
}

static uint32_t ordinary_slice_ticks(const struct process *selected) {
    if (!selected) return PROCESS_DEFAULT_QUANTUM_TICKS;
    uint64_t total_weight = rq_weight_total;
    uint64_t runnable_count = rq_ordinary;
    if (!selected->on_ready_list && !selected->rt_priority) {
        total_weight += process_weight(selected);
        runnable_count++;
    }

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
    wake_expired_timers(time_uptime_ns());
    if (!ready_processes) return NULL;

    struct process *realtime = first_allowed_rt(0, NULL);
    if (realtime) return realtime;

    (void)after;
    struct process *first = rq_first();
    uint64_t lowest = first ? first->virtual_runtime_ns : 0;
    int have_lowest = first != NULL;
    unsigned cpus = smp_cpu_count();
    for (unsigned index = 0; index < cpus && index < SMP_MAX_CPUS; index++) {
        struct cpu *cpu = percpu_slot(index);
        if (!cpu || !cpu->online) continue;
        struct process *running = cpu_running(cpu);
        if (!running || running->state != PROCESS_RUNNING || running->rt_priority) continue;
        if (!have_lowest || (int64_t)(running->virtual_runtime_ns - lowest) < 0) {
            lowest = running->virtual_runtime_ns;
            have_lowest = 1;
        }
    }
    if (have_lowest && (int64_t)(lowest - minimum_virtual_runtime) > 0)
        minimum_virtual_runtime = lowest;
    return first_allowed_ordinary(NULL);
}

static struct process *scheduling_target(uint64_t tid) {
    if (!tid) return current;
    return process_find(tid);
}

int process_set_scheduler(uint64_t tid, int policy, int rt_priority) {
    SCHED_LOCKED;
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

    int queued = target->on_ready_list;
    if (queued) ready_unlink(target);
    target->policy = policy;
    target->rt_priority = real_time ? rt_priority : 0;
    if (queued) ready_link(target);
    return 0;
}

int process_get_scheduler(uint64_t tid, int *policy, int *rt_priority) {
    SCHED_LOCKED;
    struct process *target = scheduling_target(tid);
    if (!target) return -ESRCH;
    if (policy) *policy = target->policy;
    if (rt_priority) *rt_priority = target->rt_priority;
    return 0;
}

int process_set_nice(uint64_t tid, int nice) {
    SCHED_LOCKED;
    struct process *target = scheduling_target(tid);
    if (!target) return -ESRCH;
    if (nice < -20) nice = -20;
    if (nice > 19) nice = 19;
    if (nice < target->nice && !cred_is_root()) return -EACCES;
    int queued = target->on_ready_list;
    if (queued) ready_unlink(target);
    target->nice = nice;
    if (queued) ready_link(target);
    return 0;
}

int process_get_nice(uint64_t tid, int *nice) {
    SCHED_LOCKED;
    struct process *target = scheduling_target(tid);
    if (!target) return -ESRCH;
    if (nice) *nice = target->nice;
    return 0;
}

static void online_cpu_mask(struct cpu_mask *mask) {
    memset(mask, 0, sizeof(*mask));
    unsigned cpus = smp_cpu_count();
    for (unsigned index = 0; index < cpus && index < SMP_MAX_CPUS; index++) cpu_mask_set(mask, index);
}

int process_set_affinity(uint64_t tid, const struct cpu_mask *mask) {
    SCHED_LOCKED;
    struct process *target = scheduling_target(tid);
    if (!target || !mask) return -ESRCH;
    struct cpu_mask online;
    online_cpu_mask(&online);
    struct cpu_mask wanted;
    for (unsigned word = 0; word < SMP_MAX_CPUS / 64; word++)
        wanted.bits[word] = mask->bits[word] & online.bits[word];
    if (cpu_mask_empty(&wanted)) return -EINVAL;
    target->affinity = wanted;
    target->affinity_set = 1;
    return 0;
}

int process_get_affinity(uint64_t tid, struct cpu_mask *mask) {
    SCHED_LOCKED;
    struct process *target = scheduling_target(tid);
    if (!target) return -ESRCH;
    if (!mask) return 0;
    online_cpu_mask(mask);
    if (target->affinity_set)
        for (unsigned word = 0; word < SMP_MAX_CPUS / 64; word++)
            mask->bits[word] &= target->affinity.bits[word];
    return 0;
}

static uint8_t *fpu_area(struct process *process) {
    return (uint8_t *)(((uintptr_t)process->fpu_state + 63U) & ~(uintptr_t)63U);
}

static void fpu_save(struct process *process) {
    if (!process) return;
    arch_fpu_save(fpu_area(process));
}

static void fpu_restore(struct process *process) {
    if (!process) return;
    arch_fpu_restore(fpu_area(process));
}

static void fpu_init_state(struct process *process) {
    if (!process) return;
    arch_fpu_init(fpu_area(process));
}

static void fpu_copy(struct process *destination, struct process *source) {
    memcpy(fpu_area(destination), fpu_area(source), PROCESS_FPU_STATE_SIZE);
}

static void leave_process(struct process *leaving) {
    struct cpu *cpu = cpu_current();
    if (!cpu->switching) {
        cpu->switching = 1;
        cpu->switch_owner = leaving;
    }
    if (leaving && leaving != cpu->switch_owner)
        __atomic_store_n(&leaving->on_cpu, 0, __ATOMIC_RELEASE);
}

void process_finish_switch(void) {
    struct cpu *cpu = cpu_current();
    if (!cpu->switching) return;
    struct process *owner = cpu->switch_owner;
    cpu->switching = 0;
    cpu->switch_owner = NULL;
    if (owner && owner != cpu_running(cpu))
        __atomic_store_n(&owner->on_cpu, 0, __ATOMIC_RELEASE);
}

static void activate_process(struct process *process) {
    int switching = current != process;
    if (current && switching) {
        arch_save_thread_pointers(current);
        fpu_save(current);
        leave_process(current);
    }
    __atomic_store_n(&process->on_cpu, 1, __ATOMIC_RELAXED);
    current = process;
    if (!process->time_slice_ticks)
        process->time_slice_ticks = process->rt_priority
                                      ? PROCESS_DEFAULT_QUANTUM_TICKS
                                      : ordinary_slice_ticks(process);
    process->last_scheduled_ns = time_uptime_ns();
    set_process_state(process, PROCESS_RUNNING);
    set_kernel_stack(process->kernel_stack_top);
    syscall_set_kernel_stack(process->kernel_stack_top);
    if (cpu_current()->address_space != process->cr3) {
        __atomic_store_n(&cpu_current()->address_space, process->cr3, __ATOMIC_SEQ_CST);
        vmm_activate(process->cr3);
    }
    arch_load_thread_pointers(process->fs_base, process->gs_base);
    if (switching) fpu_restore(process);
}

static void leave_for_idle(void) {
    if (current) {
        arch_save_thread_pointers(current);
        fpu_save(current);
        leave_process(current);
        current = NULL;
    }
    vmm_activate(vmm_kernel_cr3());
    cpu_current()->address_space = 0;
    set_kernel_stack(cpu_current()->idle_stack_top);
    syscall_set_kernel_stack(cpu_current()->idle_stack_top);
}

static uint64_t enter_next(struct process *next) {
    cpu_current()->in_interrupt = 0;
    if (!next) {
        leave_for_idle();
        defer_cpu_reset(0);
        return arch_context_init(cpu_current()->idle_stack_top, 0, arch_idle_entry, 0, 0);
    }
    activate_process(next);
    if (next->kernel_suspended) {
        next->kernel_suspended = 0;
        defer_unpark(&next->defer_park);
        lock_set_depth(&sched_lock, next->sched_depth);
        return next->kernel_sp;
    }
    defer_cpu_reset(1);
    return arch_context_init(next->kernel_stack_top, sizeof(struct syscall_frame),
                             arch_user_resume_entry, 0, 0);
}

static void switch_away(struct process *prev, struct process *next) {
    if (next == prev) {
        set_process_state(prev, PROCESS_RUNNING);
        return;
    }
    if (!lock_only_holds(&sched_lock)) lock_report_sleep("the scheduler");
    prev->sched_depth = lock_depth(&sched_lock);
    defer_park(&prev->defer_park);
    prev->kernel_suspended = 1;
    prev->voluntary_switches++;
    uint64_t target = enter_next(next);
    arch_switch_stack(&prev->kernel_sp, target);
}

static void abandon_to(struct process *next) __attribute__((noreturn));
static void abandon_to(struct process *next) {
    syscall_orphan_pins(current);
    uint64_t target = enter_next(next);
    arch_switch_stack(NULL, target);
    __builtin_unreachable();
}

static void resume_by_frame(struct syscall_frame *frame, struct process *next) {
    if (next->kernel_suspended) abandon_to(next);
    *frame = next->saved_frame;
    activate_process(next);
}

static int switch_to_next(struct syscall_frame *frame, struct process *after) {
    struct process *next = next_runnable(after);
    if (!next) return -1;
    resume_by_frame(frame, next);
    return 0;
}

static void go_idle(void) __attribute__((noreturn));
static void go_idle(void) {
    struct process *leaving = current;
    leave_for_idle();
    lock_drop(&sched_lock);
    syscall_orphan_pins(leaving);
    syscall_release_orphans();
    lock_check_released("the kernel for idle");
    cpu_enter_idle(cpu_current()->idle_stack_top);
}

static void after_switch(void) {
    process_finish_switch();
    syscall_release_orphans();
}

void process_idle_entry(void) {
    process_finish_switch();
    lock_drop(&sched_lock);
    syscall_release_orphans();
    lock_check_released("the kernel for idle");
}

void process_user_resume(void) {
    process_finish_switch();
    lock_drop(&sched_lock);
    syscall_release_orphans();
    struct syscall_frame resume = current->saved_frame;
    process_prepare_user_return(&resume);
    if (!current || current->state != PROCESS_RUNNING) {
        SCHED_LOCKED;
        go_idle();
    }
    current->saved_frame = resume;
    *(struct syscall_frame *)(cpu_current()->kernel_rsp - sizeof(resume)) = resume;
}

void process_kthread_start(void (*body)(void *), void *argument) {
    process_finish_switch();
    lock_drop(&sched_lock);
    syscall_release_orphans();
    body(argument);
    for (;;) {
        process_prepare_wait(current, 0);
        process_wait();
        process_finish_wait();
    }
}

int process_may_sleep(void) {
    struct process *self = current;
    if (!self || cpu_current()->in_interrupt || !lock_only_holds(NULL)) return 0;
    uint64_t here = (uint64_t)(uintptr_t)__builtin_frame_address(0);
    return here >= self->kernel_stack_base && here < self->kernel_stack_top;
}

void process_prepare_wait(const void *channel, uint64_t deadline_ns) {
    SCHED_LOCKED;
    struct process *self = current;
    if (!self) return;
    self->kernel_waiting = 1;
    self->wait_channel = channel;
    self->kernel_wait_deadline_ns = deadline_ns;
    set_process_state(self, PROCESS_BLOCKED);
    wait_link(self, (uint64_t)(uintptr_t)channel);
    if (deadline_ns) note_deadline(deadline_ns);
}

void process_wait(void) {
    if (!current) return;
    if (cpu_current()->in_interrupt || !lock_only_holds(NULL)) lock_report_sleep("a wait");
    {
        SCHED_LOCKED;
        struct process *self = current;
        if (self->state == PROCESS_BLOCKED) switch_away(self, next_runnable(self));
    }
    after_switch();
}

void process_finish_wait(void) {
    SCHED_LOCKED;
    struct process *self = current;
    if (!self) return;
    if (self->state != PROCESS_RUNNING) set_process_state(self, PROCESS_RUNNING);
    self->kernel_waiting = 0;
    self->wait_channel = NULL;
    self->kernel_wait_deadline_ns = 0;
}

void process_kernel_yield(void) {
    if (!current || !process_may_sleep()) return;
    {
        SCHED_LOCKED;
        struct process *self = current;
        set_process_state(self, PROCESS_READY);
        struct process *next = next_runnable(self);
        if (!next || next == self) {
            set_process_state(self, PROCESS_RUNNING);
            return;
        }
        switch_away(self, next);
    }
    after_switch();
}

int process_is_live(const struct process *process) {
    return process && process->state != PROCESS_ZOMBIE && process->state != PROCESS_DEAD;
}

void process_for_each(int (*visit)(struct process *process, void *context), void *context) {
    SCHED_LOCKED;
    if (!queue) return;
    struct process *item = queue;
    do {
        struct process *next = item->next;
        if (visit(item, context)) return;
        item = next;
    } while (item != queue);
}

void process_preempt_point(void) {
    struct process *self = current;
    if (!self || self->state != PROCESS_RUNNING || !process_may_sleep()) return;
    struct cpu *cpu = cpu_current();
    if (!cpu->timer_local || time_uptime_ns() < cpu->timer_programmed_ns) return;
    unsigned due = timer_local_expired();
    if (due & TIMER_LOCAL_DEADLINE) timer_run_deadlines();
    process_account_runtime();
    int yield;
    {
        SCHED_LOCKED;
        if ((due & TIMER_LOCAL_TICK) && self->time_slice_ticks) self->time_slice_ticks--;
        yield = !self->time_slice_ticks || higher_priority_waiting(self) ||
                ordinary_should_preempt(self);
    }
    if (yield) process_kernel_yield();
    SCHED_LOCKED;
    if (!self->time_slice_ticks)
        self->time_slice_ticks = self->rt_priority ? PROCESS_DEFAULT_QUANTUM_TICKS
                                                   : ordinary_slice_ticks(self);
}

void process_start_first(void) {
    go_idle();
}

void process_run_idle(void) {
    cpu_idle_park(cpu_current()->idle_stack_top);
}

static struct vm_area **area_list(void) {
    return current && current->memory ? &current->memory->areas : NULL;
}

static int area_writes_file(const struct vm_area *area) {
    return (area->page_flags & PAGE_WRITE) && !(area->kind & VM_PRIVATE);
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
        if (area_writes_file(area))
            vfs_map_write_ref(file->node, offset, end - start);
    }
    return area;
}

static void area_free(struct vm_area *area) {
    if (!area) return;
    if ((area->kind & VM_FILE_PAGES) && area->file) {
        if (area_writes_file(area)) vfs_map_write_unref(area->file->node);
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
    MEMORY_LOCKED;
    struct vm_area **list = area_list();
    if (!list || start >= end) return -1;
    process_unmap_area(start, end);
    struct vm_area *area = area_alloc(start, end, page_flags, kind, file, offset);
    if (!area) return -1;
    area_insert(list, area);
    return 0;
}

void process_unmap_area(uint64_t start, uint64_t end) {
    MEMORY_LOCKED;
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
    MEMORY_LOCKED;
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
    MEMORY_LOCKED;
    struct vm_area **list = area_list();
    if (!list || start >= end) return 0;
    for (struct vm_area *area = *list; area; area = area->next) {
        if (area->start >= end) break;
        if (area->end > start) return 0;
    }
    return 1;
}

int process_find_free_range(uint64_t start, uint64_t length, uint64_t *base_out) {
    MEMORY_LOCKED;
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
    MEMORY_LOCKED;
    struct vm_area **list = area_list();
    if (!list) return 0;
    int covered = 0;
    for (struct vm_area *area = *list; area; area = area->next) {
        if (area->start >= end) break;
        if (area->end <= start) continue;
        covered = 1;
        if ((area->kind & VM_FILE_PAGES) && area->file && area_writes_file(area))
            vfs_flush_mapped(area->file->node);
    }
    return covered;
}

struct vm_area *process_find_area(uint64_t address) {
    MEMORY_LOCKED;
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

    SCHED_LOCKED;
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
    wake_bump();
    (void)send_signal((int64_t)victim->pid, SIGKILL, 0);
    return 0;
}

static uint64_t alloc_user_page(void) {
    uint64_t physical = (uint64_t)pmm_alloc_page();
    if (!physical && reclaim_or_kill()) physical = (uint64_t)pmm_alloc_page();
    return physical;
}

#define COMMIT_AHEAD_PAGES 16ULL

static int commit_zero(uint64_t page, uint64_t flags) {
    if (vmm_translate(current->cr3, page, NULL, NULL) == 0) return 1;
    uint64_t physical = alloc_user_page();
    if (!physical) return 0;
    memset(vmm_phys_to_virt(physical), 0, 4096);
    if (vmm_map_page_in(current->cr3, page, physical, flags) != 0) {
        pmm_free_page((void *)physical);
        return 0;
    }
    return 1;
}

static int commit_one(struct vm_area *area, uint64_t page) {
    return commit_zero(page, area->page_flags);
}

static uint64_t shared_page_flags(const struct vm_area *area) {
    uint64_t flags = area->page_flags | PAGE_USER | PAGE_PRESENT | PAGE_FILEBACKED;
    if (!(area->kind & VM_PRIVATE)) return flags | PAGE_SHARED;
    if (flags & PAGE_WRITE) flags = (flags & ~PAGE_WRITE) | PAGE_COW;
    return flags;
}

static int map_shared_page(uint64_t page, uint64_t physical, uint64_t flags) {
    if (pmm_page_ref(physical) != 0) return 0;
    if (vmm_map_page_in(current->cr3, page, physical, flags) != 0) {
        pmm_free_page((void *)physical);
        return 0;
    }
    return 1;
}

static int commit_file(struct vm_area *area, uint64_t page) {
    struct file *file = area->file;
    if (!file || file->kind != FILE_KIND_VFS || !file->node) return 0;
    if (vmm_translate(current->cr3, page, NULL, NULL) == 0) return 1;
    struct vfs_node *node = file->node;
    uint64_t index = (page - area->start + area->offset) / 4096ULL;
    if (index >= (node->length + 4095ULL) / 4096ULL)
        return commit_zero(page, area->page_flags | PAGE_USER | PAGE_PRESENT);
    uint64_t window = index & ~(VFS_READAHEAD_PAGES - 1ULL);
    vfs_prefetch(node, window * 4096ULL, VFS_READAHEAD_PAGES * 4096ULL);
    uint64_t physical = vfs_page_physical(node, index) & ~0xFFFULL;
    if (!physical) return 0;
    if (pmm_page_refcount(physical) == 0 && pmm_page_ref(physical) != 0) return 0;
    return map_shared_page(page, physical, shared_page_flags(area));
}

static int commit_memfd(struct vm_area *area, uint64_t page) {
    if (!area->file || (area->file->kind != FILE_KIND_MEMFD &&
                        area->file->kind != FILE_KIND_IO_URING)) return 0;
    if (vmm_translate(current->cr3, page, NULL, NULL) == 0) return 1;
    uint64_t index = (page - area->start + area->offset) / 4096ULL;
    if (index * 4096ULL >= memfd_size(area->file->memfd)) return 0;
    uint64_t physical = memfd_page_ensure(area->file->memfd, index);
    if (!physical) return 0;
    return map_shared_page(page, physical, shared_page_flags(area));
}

int process_commit_area(uint64_t fault_address) {
    MEMORY_LOCKED;
    if (!current || current->state != PROCESS_RUNNING || !current->cr3) return 0;
    uint64_t page = fault_address & ~4095ULL;
    struct vm_area *area = process_find_area(page);
    if (area && (area->kind & VM_MEMFD)) return commit_memfd(area, page);
    if (area && (area->kind & VM_FILE_PAGES)) return commit_file(area, page);
    if (!area || !(area->kind & VM_ANONYMOUS)) return 0;
    if (!commit_one(area, page)) return 0;

    uint64_t ahead = page + 4096ULL;
    uint64_t limit = page + COMMIT_AHEAD_PAGES * 4096ULL;
    if (limit > area->end) limit = area->end;
    while (ahead < limit && commit_one(area, ahead)) ahead += 4096ULL;
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
    MEMORY_LOCKED;
    if (!current || current->state != PROCESS_RUNNING || !current->cr3) return 0;
    if (fault_address >= USER_STACK_TOP || fault_address < process_stack_floor(current)) return 0;

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
    MEMORY_LOCKED;
    if (!current || current->state != PROCESS_RUNNING || !current->cr3) return 0;
    return vmm_handle_cow_fault(current->cr3, fault_address & ~4095ULL) == 0;
}

int process_signal_has_handler(int signal_number) {
    if (!current || signal_number < 1 || signal_number > TUNIX_NSIG) return 0;
    uint64_t handler = current->signal_actions[signal_number - 1].handler;
    return handler != SIG_DFL && handler != SIG_IGN;
}

int process_fault_from_interrupt(struct interrupt_frame *frame, int signal_number) {
    if (!frame || !arch_interrupt_from_user(frame) || !current ||
        current->state != PROCESS_RUNNING) return 0;

    process_account_runtime();
    arch_frame_from_interrupt(&current->saved_frame, frame);
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
    arch_frame_to_interrupt(frame, &resume);
    return 1;
}

static int next_pending_signal(struct process *process);

static int needs_user_work(struct process *process) {
    return process->group_exit_pending ||
           (!process->in_signal && next_pending_signal(process) != 0);
}

static void resume_from_idle(struct interrupt_frame *frame) {
    struct syscall_frame resume;
    {
        SCHED_LOCKED;
        struct process *next = next_runnable(NULL);
        if (!next) return;
        if (next->kernel_suspended || needs_user_work(next)) abandon_to(next);
        activate_process(next);
        resume = next->saved_frame;
    }
    current->saved_frame = resume;
    arch_frame_to_interrupt(frame, &resume);
}

static void preempt_from_interrupt(struct interrupt_frame *frame, int tick) {
    if (!frame) return;
    if (!current) {
        resume_from_idle(frame);
        return;
    }
    if (!arch_interrupt_from_user(frame) || current->state != PROCESS_RUNNING) return;

    process_account_runtime();
    arch_frame_from_interrupt(&current->saved_frame, frame);

    struct syscall_frame resume = current->saved_frame;
    if (tick && current->time_slice_ticks) current->time_slice_ticks--;
    lock_acquire(&sched_lock);
    if ((tick && !current->time_slice_ticks) || higher_priority_waiting(current) ||
        ordinary_should_preempt(current) || !allowed_on_this_cpu(current)) {
        struct process *preempted = current;
        set_process_state(preempted, PROCESS_READY);
        struct process *next = next_runnable(preempted);
        if (next && next != preempted) {
            preempted->involuntary_switches++;
            if (next->kernel_suspended) abandon_to(next);
            resume = next->saved_frame;
            activate_process(next);
        } else {
            if (!allowed_on_this_cpu(preempted)) go_idle();
            preempted->time_slice_ticks = preempted->rt_priority
                                           ? PROCESS_DEFAULT_QUANTUM_TICKS
                                           : ordinary_slice_ticks(preempted);
            set_process_state(preempted, PROCESS_RUNNING);
            current = preempted;
        }
    }
    if (current && needs_user_work(current)) abandon_to(current);
    lock_release(&sched_lock);

    if (!current || current->state != PROCESS_RUNNING) return;
    current->saved_frame = resume;
    arch_frame_to_interrupt(frame, &resume);
}

void process_timer_interrupt(struct interrupt_frame *frame) {
    thermal_tick();
    cpufreq_tick();
    preempt_from_interrupt(frame, 1);
}

void process_deadline_interrupt(struct interrupt_frame *frame) {
    preempt_from_interrupt(frame, 0);
}

void process_reschedule_interrupt(struct interrupt_frame *frame) {
    preempt_from_interrupt(frame, 0);
}

void process_yield_from_syscall(struct syscall_frame *frame) {
    if (!current || !frame) return;
    SCHED_LOCKED;
    struct process *yielding = current;
    yielding->saved_frame = *frame;
    set_process_state(yielding, PROCESS_READY);
    struct process *next = next_runnable(yielding);
    if (!next || next == yielding) {
        if (!allowed_on_this_cpu(yielding)) go_idle();
        set_process_state(yielding, PROCESS_RUNNING);
        return;
    }
    resume_by_frame(frame, next);
}

void process_run_child_first_from_syscall(struct syscall_frame *frame, uint64_t child_pid) {
    if (!current || !frame || child_pid == 0) return;
    SCHED_LOCKED;

    struct process *parent = current;
    struct process *child = process_find(child_pid);
    if (!child || child->state != PROCESS_READY || child->on_cpu || child->kernel_suspended ||
        (child->ppid != parent->tgid &&
         !(child->is_thread && child->tgid == parent->tgid))) return;

    parent->saved_frame = *frame;
    set_process_state(parent, PROCESS_READY);
    *frame = child->saved_frame;
    activate_process(child);
}

static struct process *find_parent(struct process *child) {
    return child && child->ppid ? process_find(child->ppid) : NULL;
}

static struct process *group_leader(struct process *process) {
    if (!process || !process->is_thread) return process;
    struct process *leader = process_find(process->tgid);
    return leader ? leader : process;
}

static void wake_group_waiters(struct process *leader) {
    if (!leader || !leader->group_wait_pending || !queue) return;
    leader->group_wait_pending = 0;
    struct process *item = queue;
    do {
        if (item != leader && item->tgid == leader->tgid && item->state == PROCESS_BLOCKED) {
            if (item->wait4_active) {
                item->wait4_active = 0;
                wake_to_ready(item);
            } else if (item->wait_channel == process_io_wait_channel()) {
                item->wait_channel = NULL;
                wake_to_ready(item);
            }
        }
        item = item->next;
    } while (item != queue);
}

static int child_matches(const struct process *child, const struct process *parent, int64_t requested) {
    if (!child || !parent || child->ppid != parent->pid) return 0;
    if (requested > 0) return child->pid == (uint64_t)requested;
    if (requested == -1) return 1;
    if (requested == 0) return child->pgid == parent->pgid;
    return child->pgid == (uint64_t)(-requested);
}

static int exit_status_word(const struct process *child) {
    return child->termination_signal ? (child->termination_signal & 0x7F)
                                     : ((child->exit_status & 0xFF) << 8);
}

static int signal_reaches_waiter(const struct process *target, int signal_number) {
    if (!target || target->state != PROCESS_BLOCKED) return 0;
    if (target->signal_blocked & signal_bit(signal_number)) return 0;
    return signal_would_act(target, signal_number);
}

static void wake_waiting_parent(struct process *parent) {
    wake_group_waiters(parent);
    if (parent->wait4_active && parent->state == PROCESS_BLOCKED) {
        parent->wait4_active = 0;
        wake_to_ready(parent);
    } else if (!parent->wait4_active && signal_reaches_waiter(parent, SIGCHLD)) {
        wake_to_ready(parent);
    } else if (parent->state == PROCESS_BLOCKED && !parent->kernel_waiting &&
               ((parent->signal_waited & signal_bit(SIGCHLD)) ||
                parent->wait_channel == process_io_wait_channel())) {
        parent->wait_channel = NULL;
        wake_to_ready(parent);
    }
}

static void notify_parent_of_exit(struct process *child) {
    struct process *parent = find_parent(child);
    if (!parent) {
        mark_dead(child);
        return;
    }
    sibling_to_front(child);
    __atomic_fetch_or(&parent->signal_pending, signal_bit(SIGCHLD), __ATOMIC_RELEASE);
    wake_waiting_parent(parent);
    __atomic_store_n(&io_recheck_pending, 1, __ATOMIC_RELEASE);
}

static void notify_parent_of_job_change(struct process *child) {
    struct process *parent = find_parent(child);
    if (!parent) return;
    __atomic_fetch_or(&parent->signal_pending, signal_bit(SIGCHLD), __ATOMIC_RELEASE);
    wake_waiting_parent(parent);
}

struct linux_robust_list_head_user {
    uint64_t list_next;
    int64_t futex_offset;
    uint64_t list_op_pending;
};

static void robust_wake_address(struct process *process, uint64_t address) {
    if (!process || (address & 3U) || address >= USER_ADDRESS_LIMIT) return;
    uint32_t value;
    if (copy_from_user(&value, address, sizeof(value)) != 0) return;
    if ((value & FUTEX_TID_MASK) != (uint32_t)process->pid) return;
    value = (value & ~FUTEX_TID_MASK) | FUTEX_OWNER_DIED;
    if (copy_to_user(address, &value, sizeof(value)) != 0) return;
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
    if (copy_from_user(&head, head_address, sizeof(head)) != 0) return;

    uint64_t entry = head.list_next;
    for (unsigned count = 0; entry && entry != head_address && count < ROBUST_LIST_LIMIT; count++) {
        uint64_t next;
        if (copy_from_user(&next, entry, sizeof(next)) != 0) break;
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
    struct process *child = parent->children;
    while (child) {
        struct process *next = child->sibling_next;
        if (child != parent && child->ppid == parent->pid && child->state != PROCESS_DEAD) {
            int signal_number = child->pdeath_signal;
            child->ppid = 1;
            sibling_link(child);
            if (signal_number > 0) signal_one_process(child, signal_number);
            if (child->state == PROCESS_ZOMBIE) notify_parent_of_exit(child);
        }
        child = next;
    }
}

static void terminate_sibling_threads(int status);

static void process_exit_from_signal(struct syscall_frame *frame, int signal_number) {
    if (current && current->pid == 1)
        kprintf("TUNIX: init killed by signal %d at rip %p rsp %p\n",
                signal_number, (void *)(frame ? SYSCALL_IP(frame) : 0),
                (void *)(frame ? SYSCALL_USER_SP(frame) : 0));
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
    syscall_release_pins();
    process_handle_robust_list(exiting);
    if (exiting->clear_child_tid_user) {
        uint64_t clear_address = exiting->clear_child_tid_user;
        uint32_t zero = 0;
        (void)copy_to_user(clear_address, &zero, sizeof(zero));
        exiting->clear_child_tid_user = 0;
        (void)process_futex_wake(clear_address, 1, FUTEX_BITSET_MATCH_ANY, 1);
    }
    process_release_files(exiting);
    cgroup_exit(exiting);
    if (!exiting->is_thread)
        vt_process_exited(exiting->pid,
                          exiting->sid == exiting->pid ? exiting->sid : 0);

    SCHED_LOCKED;
    wake_bump();
    set_process_state(exiting, PROCESS_ZOMBIE);
    if (exiting->memory && !exiting->on_zombie_list) {
        exiting->on_zombie_list = 1;
        exiting->zombie_next = zombie_head;
        zombie_head = exiting;
        zombie_memory_pending = 1;
    }
    notify_children_of_parent_death(exiting);
    if (exiting->is_thread) mark_dead(exiting);
    else notify_parent_of_exit(exiting);
    KDEBUG("process: pid=%u exited status=%d\n", (unsigned)exiting->pid, status);

    if (switch_to_next(frame, exiting) != 0) go_idle();
}

int64_t process_fork_from_syscall(struct syscall_frame *frame,
                                  const struct fork_request *request) {
    if (!current || !frame) return -EINVAL;
    struct process *parent = current;
    struct process *child = (struct process *)kmalloc(sizeof(*child));
    if (!child) return -EINVAL;
    memset(child, 0, sizeof(*child));

    child->pid = __atomic_fetch_add(&next_pid, 1, __ATOMIC_RELAXED);
    child->tgid = child->pid;
    child->ppid = parent->tgid;
    child->pgid = parent->pgid;
    child->sid = parent->sid;
    child->cwd = parent->cwd;
    vfs_node_ref(child->cwd);
    child->root = parent->root;
    vfs_node_ref(child->root);
    child->controlling_pty = parent->controlling_pty;
    child->umask = parent->umask;
    child->cred = parent->cred;
    cred_groups_share(&child->cred);
    child->policy = parent->policy;
    child->rt_priority = parent->rt_priority;
    child->nice = parent->nice;
    child->virtual_runtime_ns = parent->virtual_runtime_ns;
    child->affinity = parent->affinity;
    child->affinity_set = parent->affinity_set;
    cgroup_fork(parent, child);
    child->signal_stack_pointer = parent->signal_stack_pointer;
    child->signal_stack_size = parent->signal_stack_size;
    child->signal_stack_flags = parent->signal_stack_flags;
    child->dumpable = parent->dumpable;
    child->no_new_privs = parent->no_new_privs;
    child->timerslack_ns = parent->timerslack_ns;
    child->thp_disable = parent->thp_disable;
    strncpy(child->name, parent->name, sizeof(child->name) - 1);
    child->exe_path = copy_text(parent->exe_path);
    memcpy(child->rlimits, parent->rlimits, sizeof(child->rlimits));
    process_memory_enter();
    child->cr3 = vmm_clone_address_space(parent->cr3);
    if (!child->cr3) {
        process_memory_leave();
        free_process_struct(child);
        return -EINVAL;
    }
    uint64_t parent_brk_start = parent->memory ? parent->memory->brk_start : parent->brk_start;
    uint64_t parent_brk_end = parent->memory ? parent->memory->brk_end : parent->brk_end;
    uint64_t parent_mmap_base = parent->memory ? parent->memory->mmap_base : parent->mmap_base;
    child->memory = memory_create(child->cr3, parent_brk_start, parent_brk_end,
                                  parent_mmap_base);
    if (!child->memory) {
        process_memory_leave();
        vmm_destroy_address_space(child->cr3);
        free_process_struct(child);
        return -EINVAL;
    }
    memory_copy_mappings(child->memory, parent->memory);
    process_memory_leave();
    arch_save_thread_pointers(parent);
    fpu_save(parent);
    fpu_copy(child, parent);
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
    child->arg_start = parent->arg_start;
    child->arg_end = parent->arg_end;
    child->env_end = parent->env_end;
    if (allocate_kernel_stack(child) != 0) {
        memory_unref(child->memory);
        free_process_struct(child);
        return -EINVAL;
    }

    child->saved_frame = *frame;
    SYSCALL_RET(&child->saved_frame) = 0;
    child->signal_blocked = parent->signal_blocked;
    memcpy(child->signal_actions, parent->signal_actions, sizeof(child->signal_actions));

    child->files = file_table_clone(parent->files);
    if (!child->files) {
        memory_unref(child->memory);
        kfree((void *)child->kernel_stack_base);
        free_process_struct(child);
        return -EAGAIN;
    }

    if (request) {
        if (request->child_stack) SYSCALL_USER_SP(&child->saved_frame) = request->child_stack;
        if (request->child_settid_user) {
            uint32_t tid = (uint32_t)child->pid;
            (void)vmm_copy_to_space(child->cr3, request->child_settid_user, &tid, sizeof(tid));
        }
        if (request->child_cleartid_user) child->clear_child_tid_user = request->child_cleartid_user;
        if (request->clear_signal_handlers) {
            for (unsigned signal = 0; signal < TUNIX_NSIG; signal++)
                if (child->signal_actions[signal].handler != SIG_IGN)
                    memset(&child->signal_actions[signal], 0, sizeof(child->signal_actions[signal]));
        }
    }

    {
        SCHED_LOCKED;
        wake_bump();
        enqueue(child);
        ready_link(child);
    }
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

    child->pid = __atomic_fetch_add(&next_pid, 1, __ATOMIC_RELAXED);
    child->tgid = parent->tgid;
    child->ppid = parent->ppid;
    child->pgid = parent->pgid;
    child->sid = parent->sid;
    child->is_thread = 1;
    child->cwd = parent->cwd;
    vfs_node_ref(child->cwd);
    child->root = parent->root;
    vfs_node_ref(child->root);
    child->controlling_pty = parent->controlling_pty;
    child->umask = parent->umask;
    child->cred = parent->cred;
    cred_groups_share(&child->cred);
    child->policy = parent->policy;
    child->rt_priority = parent->rt_priority;
    child->nice = parent->nice;
    child->virtual_runtime_ns = parent->virtual_runtime_ns;
    child->affinity = parent->affinity;
    child->affinity_set = parent->affinity_set;
    cgroup_fork(parent, child);
    child->signal_stack_flags = SS_DISABLE;
    child->dumpable = parent->dumpable;
    child->no_new_privs = parent->no_new_privs;
    child->timerslack_ns = parent->timerslack_ns;
    child->thp_disable = parent->thp_disable;
    strncpy(child->name, parent->name, sizeof(child->name) - 1);
    child->exe_path = copy_text(parent->exe_path);
    memcpy(child->rlimits, parent->rlimits, sizeof(child->rlimits));
    child->memory = parent->memory;
    memory_ref(child->memory);
    sync_memory_view(child);
    arch_save_thread_pointers(parent);
    fpu_save(parent);
    fpu_copy(child, parent);
    child->entry = parent->entry;
    child->user_stack_top = child_stack;
    child->fs_base = (flags & 0x00080000ULL) ? tls : parent->fs_base;
    child->gs_base = parent->gs_base;
    child->start_time_ns = time_uptime_ns();
    child->arg_start = parent->arg_start;
    child->arg_end = parent->arg_end;
    child->env_end = parent->env_end;
    if (allocate_kernel_stack(child) != 0) {
        memory_unref(child->memory);
        free_process_struct(child);
        return -EINVAL;
    }

    child->saved_frame = *frame;
    SYSCALL_RET(&child->saved_frame) = 0;
    SYSCALL_USER_SP(&child->saved_frame) = child_stack;
    child->signal_blocked = parent->signal_blocked;
    memcpy(child->signal_actions, parent->signal_actions, sizeof(child->signal_actions));

    child->files = parent->files;
    file_table_ref(child->files);

    uint32_t tid = (uint32_t)child->pid;
    if ((flags & 0x00100000ULL) && parent_tid_user &&
        copy_to_user(parent_tid_user, &tid, sizeof(tid)) != 0) {
        process_release_files(child);
        memory_unref(child->memory);
        kfree((void *)child->kernel_stack_base);
        free_process_struct(child);
        return -EFAULT;
    }
    if ((flags & (0x01000000ULL | 0x00200000ULL)) && child_tid_user) {
        if ((flags & 0x01000000ULL) &&
            copy_to_user(child_tid_user, &tid, sizeof(tid)) != 0) {
            process_release_files(child);
            memory_unref(child->memory);
            kfree((void *)child->kernel_stack_base);
            free_process_struct(child);
            return -EFAULT;
        }
        if (flags & 0x00200000ULL) child->clear_child_tid_user = child_tid_user;
    }

    {
        SCHED_LOCKED;
        wake_bump();
        enqueue(child);
        ready_link(child);
    }
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
    if (copy_from_user(&value, address, sizeof(value)) != 0)
        return -EFAULT;
    if (value != expected) { futex_note('A', address, 0, 0, value); return -EAGAIN; }
    if (timeout_ns == 0) return -ETIMEDOUT;
    uint64_t key = shared ? futex_shared_key(address) : 0;
    SCHED_LOCKED;
    if (process_signal_interrupts_wait()) return -EINTR;
    if (wake_missed()) return 0;

    struct process *waiting = current;
    waiting->saved_frame = *frame;
    SYSCALL_RET(&waiting->saved_frame) = 0;
    set_process_state(waiting, PROCESS_BLOCKED);
    waiting->futex_wait_active = 1;
    waiting->futex_wait_address = address;
    waiting->futex_wait_key = key;
    waiting->futex_wait_expected = expected;
    waiting->futex_wait_bitset = bitset;
    futex_note('W', address, 0, 0, expected);
    waiting->futex_wait_deadline_ns = timeout_ns < 0 ? UINT64_MAX :
        time_uptime_ns() + (uint64_t)timeout_ns;
    wait_link(waiting, address);
    key_link(waiting, waiting->futex_wait_key);
    if (timeout_ns >= 0) note_deadline(waiting->futex_wait_deadline_ns);
    waiting->voluntary_switches++;
    if (switch_to_next(frame, waiting) != 0) go_idle();
    return 0;
}

static volatile uint32_t *user_futex_word(uint64_t address) {
    if (!current || (address & 3U) || address >= USER_ADDRESS_LIMIT) return NULL;
    uint32_t probe;
    if (copy_from_user(&probe, address, sizeof(probe)) != 0) return NULL;
    uint64_t physical = 0, flags = 0;
    if (vmm_translate(current->cr3, address, &physical, &flags) != 0) return NULL;
    if (!(flags & PAGE_WRITE)) {
        if (!process_handle_cow_fault(address)) return NULL;
        if (vmm_translate(current->cr3, address, &physical, &flags) != 0 || !(flags & PAGE_WRITE))
            return NULL;
    }
    return (volatile uint32_t *)vmm_phys_to_virt(physical);
}

static int pi_sleep(struct syscall_frame *frame, uint64_t address, uint32_t expected,
                    volatile uint32_t *word, int64_t timeout_ns, int shared,
                    uint64_t syscall_number) {
    uint64_t key = shared ? futex_shared_key(address) : 0;
    SCHED_LOCKED;
    if (__atomic_load_n(word, __ATOMIC_ACQUIRE) != expected) return 0;
    struct process *waiting = current;
    SYSCALL_RESTART(frame, syscall_number);
    waiting->syscall_rewound = 1;
    waiting->syscall_force_restart = 1;
    waiting->saved_frame = *frame;
    set_process_state(waiting, PROCESS_BLOCKED);
    waiting->futex_wait_active = 1;
    waiting->futex_wait_address = address;
    waiting->futex_wait_key = key;
    waiting->futex_wait_expected = expected;
    waiting->futex_wait_bitset = FUTEX_BITSET_MATCH_ANY;
    waiting->futex_wait_deadline_ns = timeout_ns < 0 ? UINT64_MAX :
        time_uptime_ns() + (uint64_t)timeout_ns;
    wait_link(waiting, address);
    key_link(waiting, key);
    if (timeout_ns >= 0) note_deadline(waiting->futex_wait_deadline_ns);
    waiting->voluntary_switches++;
    if (switch_to_next(frame, waiting) != 0) go_idle();
    return 1;
}

int64_t process_futex_lock_pi(struct syscall_frame *frame, uint64_t address,
                              int64_t deadline_ns, int trylock, int shared,
                              uint64_t syscall_number) {
    volatile uint32_t *word = user_futex_word(address);
    if (!word) return -EFAULT;
    uint32_t tid = (uint32_t)current->pid;
    for (;;) {
        uint32_t value = __atomic_load_n(word, __ATOMIC_ACQUIRE);
        uint32_t owner = value & FUTEX_TID_MASK;
        if (!owner) {
            uint32_t desired = tid | (value & (FUTEX_WAITERS | FUTEX_OWNER_DIED));
            if (__atomic_compare_exchange_n(word, &value, desired, 0, __ATOMIC_ACQ_REL,
                                            __ATOMIC_ACQUIRE)) {
                current->syscall_force_restart = 0;
                return 0;
            }
            continue;
        }
        if (owner == tid) {
            current->syscall_force_restart = 0;
            return -EDEADLK;
        }
        int holder_alive;
        {
            SCHED_LOCKED;
            struct process *holder = process_find(owner);
            holder_alive = holder && holder->state != PROCESS_DEAD;
        }
        if (!holder_alive) {
            current->syscall_force_restart = 0;
            return -ESRCH;
        }
        if (trylock) return -EAGAIN;
        if (!(value & FUTEX_WAITERS)) {
            if (!__atomic_compare_exchange_n(word, &value, value | FUTEX_WAITERS, 0,
                                             __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
                continue;
            value |= FUTEX_WAITERS;
        }
        int64_t timeout_ns = -1;
        if (deadline_ns >= 0) {
            uint64_t now = time_realtime_ns();
            if (now >= (uint64_t)deadline_ns) {
                current->syscall_force_restart = 0;
                return -ETIMEDOUT;
            }
            timeout_ns = (int64_t)((uint64_t)deadline_ns - now);
        }
        if (pi_sleep(frame, address, value, word, timeout_ns, shared, syscall_number))
            return PROCESS_RESTARTED;
    }
}

int64_t process_futex_unlock_pi(uint64_t address, int shared) {
    volatile uint32_t *word = user_futex_word(address);
    if (!word) return -EFAULT;
    uint32_t value = __atomic_load_n(word, __ATOMIC_ACQUIRE);
    if ((value & FUTEX_TID_MASK) != (uint32_t)current->pid) return -EPERM;
    __atomic_store_n(word, 0, __ATOMIC_RELEASE);
    if (value & FUTEX_WAITERS)
        (void)process_futex_wake(address, INT32_MAX, FUTEX_BITSET_MATCH_ANY, shared);
    return 0;
}

static int wake_op_compare(int cmp, int32_t value, int32_t argument) {
    switch (cmp) {
    case 0: return value == argument;
    case 1: return value != argument;
    case 2: return value < argument;
    case 3: return value <= argument;
    case 4: return value > argument;
    case 5: return value >= argument;
    default: return 0;
    }
}

int64_t process_futex_wake_op(uint64_t address, int wake, uint64_t second, int wake_second,
                              uint32_t encoded, int shared) {
    unsigned op = (encoded >> 28) & 7U;
    unsigned cmp = (encoded >> 24) & 15U;
    uint32_t oparg = (encoded >> 12) & 0xFFFU;
    int32_t cmparg = (int32_t)(encoded & 0xFFFU);
    if (encoded & 0x80000000U) oparg = 1U << (oparg & 31U);
    if (cmp > 5U || op > 4U) return -EINVAL;
    volatile uint32_t *word = user_futex_word(second);
    if (!word) return -EFAULT;
    uint32_t old = __atomic_load_n(word, __ATOMIC_ACQUIRE);
    for (;;) {
        uint32_t next = op == 0 ? oparg : op == 1 ? old + oparg : op == 2 ? (old | oparg)
                      : op == 3 ? (old & ~oparg) : (old ^ oparg);
        if (__atomic_compare_exchange_n(word, &old, next, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            break;
    }
    int64_t woken = process_futex_wake(address, wake, FUTEX_BITSET_MATCH_ANY, shared);
    if (wake_op_compare((int)cmp, (int32_t)old, cmparg))
        woken += process_futex_wake(second, wake_second, FUTEX_BITSET_MATCH_ANY, shared);
    return woken;
}

int process_sleep_on(struct syscall_frame *frame, const void *channel) {
    if (!current || !frame || !channel) return -EAGAIN;
    SCHED_LOCKED;
    if (wake_missed()) return -EAGAIN;
    struct process *waiting = current;
    waiting->saved_frame = *frame;
    set_process_state(waiting, PROCESS_BLOCKED);
    waiting->wait_channel = channel;
    wait_link(waiting, (uint64_t)(uintptr_t)channel);
    waiting->voluntary_switches++;
    if (switch_to_next(frame, waiting) != 0) {
        go_idle();
    }
    return 0;
}

static const char io_wait_token;

const void *process_io_wait_channel(void) { return &io_wait_token; }

int process_wake_io(void) { return process_wake_all(&io_wait_token); }

int process_wake_all(const void *channel) {
    SCHED_LOCKED;
    wake_bump();
    return process_wake_all_locked(channel);
}

int process_wake_one(const void *channel) {
    SCHED_LOCKED;
    wake_bump();
    struct process *item = wait_buckets[wait_bucket_of((uint64_t)(uintptr_t)channel)];
    for (; item; item = item->wait_next) {
        if (item->state != PROCESS_BLOCKED || item->wait_channel != channel) continue;
        item->wait_channel = NULL;
        wake_to_ready(item);
        return 1;
    }
    return 0;
}

static int io_waiter_urgent(const struct process *item, uint64_t *now) {
    if (!item->io_watch_armed) return 1;
    if (__atomic_load_n(&item->signal_pending, __ATOMIC_RELAXED) & ~item->signal_blocked)
        return 1;
    if (item->io_wait_active && item->io_wait_deadline_ns != UINT64_MAX) {
        if (!*now) *now = time_uptime_ns();
        if (*now >= item->io_wait_deadline_ns) return 1;
    }
    return 0;
}

static int wake_bucket(const void *channel, const void *exact, uint64_t *now) {
    int woken = 0;
    struct process *item = wait_buckets[wait_bucket_of((uint64_t)(uintptr_t)channel)];
    while (item) {
        struct process *next = item->wait_next;
        if (item->state == PROCESS_BLOCKED && item->wait_channel) {
            if (exact && item->wait_channel == exact) {
                item->wait_channel = NULL;
                wake_to_ready(item);
                woken++;
            } else if (!exact && item->wait_channel == &io_wait_token) {
                if (io_waiter_urgent(item, now)) {
                    item->wait_channel = NULL;
                    wake_to_ready(item);
                    woken++;
                } else {
                    if (item->io_wait_active) timer_note_deadline(item->io_wait_deadline_ns);
                    __atomic_store_n(&io_recheck_pending, 1, __ATOMIC_RELEASE);
                }
            }
        }
        item = next;
    }
    return woken;
}

static int process_wake_all_locked(const void *channel) {
    if (!queue || !channel) return 0;
    uint64_t now = 0;
    int woken = 0;
    if (channel != &io_wait_token) woken += wake_bucket(channel, channel, &now);
    woken += wake_bucket(&io_wait_token, NULL, &now);
    return woken;
}

static struct process *poll_subjects[SMP_MAX_CPUS];

struct process *process_poll_subject(void) {
    struct process *subject = poll_subjects[cpu_current()->index];
    return subject ? subject : current;
}

static int io_files_ready(struct process *item, struct file_table *table) {
    if (!table) return 1;
    for (unsigned index = 0; index < item->io_watch_count; index++) {
        struct file *file = file_table_get(table, item->io_watch_fd[index]);
        if (!file) return 1;
        uint32_t events = file_poll_events(file, item->io_watch_events[index]);
        file_unref(file);
        if (events) return 1;
    }
    return 0;
}

static uint64_t io_recheck_epoch = 1;
static uint32_t io_recheck_running;

static int io_recheck_listed(struct process **candidates, unsigned count, const struct process *item) {
    for (unsigned index = 0; index < count; index++)
        if (candidates[index] == item) return 1;
    return 0;
}

static unsigned io_recheck_batch(struct process **candidates, struct file_table **tables,
                                 unsigned limit, int *more) {
    SCHED_LOCKED;
    unsigned count = 0;
    *more = 0;
    for (int pass = 0; pass < 2 && !*more; pass++) {
        struct process *item = wait_buckets[wait_bucket_of((uint64_t)(uintptr_t)&io_wait_token)];
        for (; item; item = item->wait_next) {
            if (item->state != PROCESS_BLOCKED || item->wait_channel != &io_wait_token) continue;
            if (item->io_recheck_epoch == io_recheck_epoch) continue;
            if (pass && io_recheck_listed(candidates, count, item)) continue;
            if (count == limit) {
                *more = 1;
                break;
            }
            item->io_recheck_epoch = io_recheck_epoch;
            item->refs++;
            tables[count] = item->files;
            if (tables[count]) file_table_ref(tables[count]);
            candidates[count++] = item;
        }
        if (!*more && !pass) io_recheck_epoch++;
    }
    return count;
}

void process_io_recheck(void) {
    if (!__atomic_load_n(&io_recheck_pending, __ATOMIC_ACQUIRE)) return;
    if (__atomic_exchange_n(&io_recheck_running, 1, __ATOMIC_ACQ_REL)) return;
    __atomic_store_n(&io_recheck_pending, 0, __ATOMIC_RELEASE);
    struct process *candidates[64];
    struct file_table *tables[64];
    int more = 0;
    unsigned count = io_recheck_batch(candidates, tables, 64, &more);
    if (more) __atomic_store_n(&io_recheck_pending, 1, __ATOMIC_RELEASE);
    uint8_t ready[64];
    for (unsigned index = 0; index < count; index++) {
        poll_subjects[cpu_current()->index] = candidates[index];
        ready[index] = (uint8_t)io_files_ready(candidates[index], tables[index]);
    }
    poll_subjects[cpu_current()->index] = NULL;
    {
        SCHED_LOCKED;
        for (unsigned index = 0; index < count; index++) {
            struct process *item = candidates[index];
            if (ready[index] && item->state == PROCESS_BLOCKED &&
                item->wait_channel == &io_wait_token) {
                item->wait_channel = NULL;
                wake_to_ready(item);
            }
        }
    }
    for (unsigned index = 0; index < count; index++) {
        file_table_unref_deferred(tables[index]);
        process_put(candidates[index]);
    }
    __atomic_store_n(&io_recheck_running, 0, __ATOMIC_RELEASE);
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

static int futex_wake_list(struct process *item, int by_key, uint64_t address, uint64_t key,
                           uint32_t bitset, int maximum, int woken) {
    while (item && woken < maximum) {
        struct process *next = by_key ? item->key_next : item->wait_next;
        int named = (key && item->futex_wait_key == key) ||
                    (item->memory == current->memory && item->futex_wait_address == address);
        if (item->state == PROCESS_BLOCKED && item->futex_wait_active && named &&
            (item->futex_wait_bitset & bitset)) {
            item->futex_wait_active = 0;
            item->futex_wait_address = 0;
            item->futex_wait_key = 0;
            item->futex_wait_deadline_ns = 0;
            if (!item->syscall_rewound) SYSCALL_RET(&item->saved_frame) = 0;
            wake_to_ready(item);
            woken++;
        }
        item = next;
    }
    return woken;
}

int process_futex_wake(uint64_t address, int maximum, uint32_t bitset, int shared) {
    if (!current || maximum <= 0 || !bitset) return 0;
    uint64_t key = shared ? futex_shared_key(address) : 0;
    SCHED_LOCKED;
    wake_bump();
    if (!queue) return 0;
    int woken = futex_wake_list(wait_buckets[wait_bucket_of(address)], 0, address, key,
                                bitset, maximum, 0);
    if (key && woken < maximum)
        woken = futex_wake_list(key_buckets[wait_bucket_of(key)], 1, address, key,
                                bitset, maximum, woken);
    futex_note('K', address, woken, maximum, 0);
    return woken;
}

void process_expire_deadlines(void) {
    SCHED_LOCKED;
    wake_expired_timers(time_uptime_ns());
    timer_note_deadline(earliest_deadline);
}

int process_ready_pending(void) {
    if (__atomic_load_n(&ready_processes, __ATOMIC_RELAXED)) return 1;
    uint64_t deadline = __atomic_load_n(&earliest_deadline, __ATOMIC_RELAXED);
    return deadline != UINT64_MAX && time_uptime_ns() >= deadline;
}

void process_note_deadline(uint64_t deadline_ns) {
    SCHED_LOCKED;
    note_deadline(deadline_ns);
}

void process_set_sigaction(int signal_number,
                           const struct tunix_sigaction *action) {
    if (!current || signal_number < 1 || signal_number > TUNIX_NSIG) return;
    current->signal_actions[signal_number - 1] = *action;
    arch_sanitize_sigaction(&current->signal_actions[signal_number - 1]);
    action = &current->signal_actions[signal_number - 1];
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
    if (!current) return;
    SCHED_LOCKED;
    if (!queue) return;
    wake_bump();
    uint64_t group = current->tgid;
    struct process *item = queue;
    do {
        if (item != current && item->tgid == group && item->state != PROCESS_DEAD &&
            item->state != PROCESS_ZOMBIE) {
            item->exit_status = status;
            item->is_thread = 1;
            item->group_exit_pending = 1;
            if ((item->state == PROCESS_BLOCKED && !item->kernel_waiting) ||
                item->state == PROCESS_STOPPED) {
                item->futex_wait_active = 0;
                item->wait4_active = 0;
                item->wait_channel = NULL;
                wake_to_ready(item);
            }
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
    image.root = current->root;
    memcpy(image.rlimits, current->rlimits, sizeof(image.rlimits));
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
    terminate_sibling_threads(0);
    struct process_memory *old_memory = current->memory;
    uint64_t old_cr3 = current->cr3;
    current->memory = new_memory;
    current->cr3 = new_cr3;
    {
        SCHED_LOCKED;
        current->tgid = current->pid;
        current->is_thread = 0;
    }
    current->entry = image.entry;
    fpu_init_state(current);
    fpu_restore(current);
    current->user_stack_top = image.user_stack_top;
    current->brk_start = image.brk_start;
    current->brk_end = image.brk_end;
    current->mmap_base = image.mmap_base;
    current->arg_start = image.arg_start;
    current->arg_end = image.arg_end;
    current->env_end = image.env_end;
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
    for (int sig = 0; sig < TUNIX_NSIG; sig++) {
        if (current->signal_actions[sig].handler != SIG_IGN) memset(&current->signal_actions[sig], 0, sizeof(current->signal_actions[sig]));
    }
    __atomic_store_n(&current->signal_pending, 0, __ATOMIC_RELEASE);
    current->in_signal = 0;
    file_table_close_on_exec(current->files);

    memset(frame, 0, sizeof(*frame));
    arch_frame_enter_user(frame, current->entry, current->user_stack_top);
    __atomic_store_n(&cpu_current()->address_space, new_cr3, __ATOMIC_SEQ_CST);
    vmm_activate(new_cr3);
    arch_load_thread_pointers(0, 0);
    if (old_memory) memory_unref(old_memory);
    else vmm_destroy_address_space(old_cr3);
    eventfs_emit_process_exec(current->cred.euid, current->pid, current->name);
    KDEBUG("process: pid=%u exec %s\n", (unsigned)current->pid, path);
    return 0;
}

int64_t process_waitpid_from_syscall(struct syscall_frame *frame, int64_t pid,
                                     uint64_t status_user, int options,
                                     uint64_t syscall_number) {
    options &= ~(WNOTHREAD | WALLCHILDREN | WCLONE);
    if (!current || !frame || (options & ~(WNOHANG | WUNTRACED | WCONTINUED))) return -EINVAL;
    struct process *caller = current;
    struct process *parent;
    int status = 0;
    int64_t found = 0;
    {
        SCHED_LOCKED;
        parent = group_leader(caller);
        int has_child = 0;
        for (struct process *item = parent->children; item && !found;
             item = item->sibling_next) {
            if (!child_matches(item, parent, pid)) continue;
            has_child = 1;
            if (item->state == PROCESS_ZOMBIE) {
                status = exit_status_word(item);
                found = (int64_t)item->pid;
                mark_dead(item);
            } else if ((options & WUNTRACED) && item->state == PROCESS_STOPPED &&
                       !item->stop_reported) {
                status = ((item->stop_signal & 0xFF) << 8) | 0x7F;
                item->stop_reported = 1;
                found = (int64_t)item->pid;
            } else if ((options & WCONTINUED) && item->continued_pending) {
                status = 0xFFFF;
                item->continued_pending = 0;
                found = (int64_t)item->pid;
            }
        }
        if (!found) {
            if (!has_child) return -ECHILD;
            if (options & WNOHANG) return 0;
            if (process_signal_interrupts_wait()) return -EINTR;
            SYSCALL_RESTART(frame, syscall_number);
            caller->syscall_rewound = 1;
            caller->saved_frame = *frame;
            if (wake_missed()) {
                set_process_state(caller, PROCESS_READY);
                struct process *next = next_runnable(caller);
                if (!next || next == caller) {
                    set_process_state(caller, PROCESS_RUNNING);
                    return PROCESS_RESTARTED;
                }
                resume_by_frame(frame, next);
                return PROCESS_RESTARTED;
            }
            if (caller != parent) parent->group_wait_pending = 1;
            set_process_state(caller, PROCESS_BLOCKED);
            caller->wait4_active = 1;
            caller->wait_pid = pid;
            caller->wait_options = options;
            caller->voluntary_switches++;
            if (switch_to_next(frame, caller) != 0) go_idle();
            return PROCESS_RESTARTED;
        }
    }
    if (status_user && copy_to_user(status_user, &status, sizeof(status)) != 0)
        return -EFAULT;
    return found;
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
    struct process *parent;
    struct waitid_siginfo info;
    memset(&info, 0, sizeof(info));
    int found = 0;
    {
        SCHED_LOCKED;
        parent = group_leader(current);
        int has_child = 0;
        for (struct process *item = parent->children; item && !found;
             item = item->sibling_next) {
            if (!child_matches(item, parent, pid_spec)) continue;
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
                if (!(options & WNOWAIT)) mark_dead(item);
                found = 1;
            } else if ((options & WSTOPPED) && item->state == PROCESS_STOPPED &&
                       !item->stop_reported) {
                info.si_signo = SIGCHLD;
                info.si_pid = (int32_t)item->pid;
                info.si_code = CLD_STOPPED;
                info.si_status = item->stop_signal & 0xFF;
                if (!(options & WNOWAIT)) item->stop_reported = 1;
                found = 1;
            } else if ((options & WCONTINUED) && item->continued_pending) {
                info.si_signo = SIGCHLD;
                info.si_pid = (int32_t)item->pid;
                info.si_code = CLD_CONTINUED;
                info.si_status = SIGCONT;
                if (!(options & WNOWAIT)) item->continued_pending = 0;
                found = 1;
            }
        }
        if (!found) {
            if (!has_child) return -ECHILD;
            if (!(options & WNOHANG)) {
                if (current != parent) parent->group_wait_pending = 1;
                return -EAGAIN;
            }
        }
    }
    if (copy_to_user(info_user, &info, sizeof(info)) != 0) return -EFAULT;
    return 0;
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
        __atomic_fetch_and(&target->signal_pending,
                           ~(signal_bit(SIGSTOP) | signal_bit(SIGTSTP) |
                             signal_bit(SIGTTIN) | signal_bit(SIGTTOU)), __ATOMIC_RELEASE);
        notify_parent_of_job_change(target);
    }
    __atomic_fetch_or(&target->signal_pending, signal_bit(signal_number), __ATOMIC_RELEASE);
    if (signal_number == SIGCHLD && target->state == PROCESS_BLOCKED &&
        target->wait_channel == &io_wait_token && !(target->signal_waited & signal_bit(SIGCHLD)) &&
        !signal_reaches_waiter(target, SIGCHLD)) {
        target->wait_channel = NULL;
        wake_to_ready(target);
        return;
    }
    if (target->state == PROCESS_BLOCKED && !target->kernel_waiting &&
        (signal_number != SIGCHLD || (target->signal_waited & signal_bit(SIGCHLD)) ||
         (!target->wait4_active && signal_reaches_waiter(target, SIGCHLD)))) {
        target->futex_wait_active = 0;
        target->futex_wait_address = 0;
        target->futex_wait_key = 0;
        target->futex_wait_deadline_ns = 0;
        if (!target->syscall_rewound)
            SYSCALL_RET(&target->saved_frame) = (uint64_t)-(int64_t)EINTR;
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

static void record_sender_code(struct process *target, int signal_number, int code) {
    if (signal_number < 1 || signal_number > TUNIX_NSIG) return;
    target->signal_sender_pid[signal_number - 1] = (uint32_t)current->tgid;
    target->signal_sender_uid[signal_number - 1] = current->cred.uid;
    target->signal_sender_code[signal_number - 1] = (int8_t)code;
    __atomic_fetch_or(&target->signal_user_sent, signal_bit(signal_number), __ATOMIC_RELEASE);
}

static void record_sender(struct process *target, int signal_number) {
    record_sender_code(target, signal_number, SI_USER);
}

int64_t process_send_thread_signal(int64_t tgid, int64_t tid, int signal_number) {
    if (signal_number < 0 || signal_number > TUNIX_NSIG || tid <= 0) return -EINVAL;
    if (!current) return -EINVAL;
    SCHED_LOCKED;
    wake_bump();
    struct process *target = process_find((uint64_t)tid);
    if (!target || target->state == PROCESS_DEAD) return -ESRCH;
    if (tgid > 0 && target->tgid != (uint64_t)tgid) return -ESRCH;
    if (!may_signal(target)) return -EPERM;
    if (target->is_kthread || !signal_number) return 0;
    record_sender_code(target, signal_number, SI_TKILL);
    signal_one_process(target, signal_number);
    return 0;
}

static int send_signal(int64_t pid, int signal_number, int checked) {
    if (signal_number < 0 || signal_number > TUNIX_NSIG) return -EINVAL;
    if ((checked || pid == 0) && !current) return -EINVAL;
    if (pid > 0) {
        struct process *target = process_find((uint64_t)pid);
        if (!target) return -ESRCH;
        if (target->is_kthread) return checked && !may_signal(target) ? -EPERM : 0;
        if (checked && !may_signal(target)) return -EPERM;
        if (checked) record_sender(target, signal_number);
        signal_one_process(target, signal_number);
        return 0;
    }

    uint64_t group = pid == 0 ? current->pgid : (uint64_t)(-pid);
    int delivered = 0;
    int refused = 0;
    if (!queue) return -ESRCH;
    struct process *target = queue;
    do {
        int match = pid == -1 ? target->pid != 1 : target->pgid == group;
        if (match && target->state != PROCESS_DEAD && !target->is_kthread) {
            if (checked && !may_signal(target)) refused = 1;
            else {
                if (checked) record_sender(target, signal_number);
                signal_one_process(target, signal_number);
                delivered = 1;
            }
        }
        target = target->next;
    } while (target != queue);
    if (delivered) return 0;
    return refused ? -EPERM : -ESRCH;
}

int process_send_signal(int64_t pid, int signal_number) {
    SCHED_LOCKED;
    wake_bump();
    return send_signal(pid, signal_number, 0);
}

int process_send_signal_checked(int64_t pid, int signal_number) {
    SCHED_LOCKED;
    wake_bump();
    return send_signal(pid, signal_number, 1);
}

int process_setpgid(int64_t pid, int64_t pgid) {
    if (!current) return -EINVAL;
    SCHED_LOCKED;
    struct process *target = pid == 0 ? current : process_find((uint64_t)pid);
    if (!target) return -ESRCH;
    if (target != current && target->ppid != current->tgid) return -EPERM;
    if (pgid == 0) pgid = (int64_t)target->pid;
    if (pgid < 0) return -EINVAL;
    target->pgid = (uint64_t)pgid;
    return 0;
}

int64_t process_setsid(void) {
    if (!current) return -EINVAL;
    SCHED_LOCKED;
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
        if (copy_from_user(context, current->signal_context_address,
                           SIGNAL_CONTEXT_SIZE) == 0)
            read_user_context(frame, context);
    }
    current->signal_context_address = 0;
    current->signal_blocked = current->signal_saved_mask;
    current->in_signal = 0;
    return 0;
}

static int next_pending_signal(struct process *process) {
    uint64_t pending = __atomic_load_n(&process->signal_pending, __ATOMIC_ACQUIRE);
    uint64_t available = pending & ~process->signal_blocked;
    available |= pending & signal_bit(SIGKILL);
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

struct vfs_node *process_get_root(void) {
    if (current && current->root) return current->root;
    return vfs_root;
}

void process_set_root(struct vfs_node *node) {
    if (!current) return;
    struct vfs_node *previous = current->root;
    vfs_node_ref(node);
    current->root = node;
    vfs_node_unref(previous);
}

void process_swap_signal_mask(uint64_t mask) {
    if (!current) return;
    if (!current->signal_wait_mask_active) {
        current->signal_wait_mask_saved = current->signal_blocked;
        current->signal_wait_mask_active = 1;
    }
    current->signal_blocked = mask & ~(signal_bit(SIGKILL) | signal_bit(SIGSTOP));
}

void process_restore_signal_mask(void) {
    if (!current || !current->signal_wait_mask_active) return;
    if (!current->in_signal && signal_would_act(current, next_pending_signal(current))) return;
    current->signal_blocked = current->signal_wait_mask_saved;
    current->signal_wait_mask_active = 0;
}

static uint64_t take_resume_mask(void) {
    if (!current->signal_wait_mask_active) return current->signal_blocked;
    current->signal_wait_mask_active = 0;
    return current->signal_wait_mask_saved;
}

int process_take_signal(uint64_t set, int32_t *info) {
    struct process *self = current;
    if (!self) return 0;
    SCHED_LOCKED;
    uint64_t pending = __atomic_load_n(&self->signal_pending, __ATOMIC_ACQUIRE) & set;
    if (!pending) return 0;
    int signal_number = __builtin_ctzll(pending) + 1;
    uint64_t bit = signal_bit(signal_number);
    __atomic_fetch_and(&self->signal_pending, ~bit, __ATOMIC_ACQ_REL);
    memset(info, 0, SIGNAL_SIGINFO_SIZE);
    info[0] = signal_number;
    int from_user = (self->signal_user_sent & bit) != 0;
    info[2] = from_user ? self->signal_sender_code[signal_number - 1] : SI_KERNEL;
    if (from_user) {
        info[4] = (int32_t)self->signal_sender_pid[signal_number - 1];
        info[5] = (int32_t)self->signal_sender_uid[signal_number - 1];
    }
    self->signal_user_sent &= ~bit;
    self->signal_sender_pid[signal_number - 1] = 0;
    self->signal_sender_uid[signal_number - 1] = 0;
    return signal_number;
}

int process_signal_interrupts_wait(void) {
    if (!current || current->in_signal) return 0;
    if (!current->group_exit_pending &&
        !signal_would_act(current, next_pending_signal(current))) return 0;
    current->syscall_rewound = 0;
    return 1;
}

static void fill_user_context(uint8_t *context, const struct syscall_frame *frame,
                              uint64_t blocked) {
    arch_fill_mcontext(context, frame);
    memcpy(context + UCONTEXT_SIGMASK_OFFSET, &blocked, sizeof(blocked));
    uint64_t stack_pointer = current ? current->signal_stack_pointer : 0;
    uint64_t stack_size = current ? current->signal_stack_size : 0;
    uint32_t stack_flags = current ? (uint32_t)current->signal_stack_flags : SS_DISABLE;
    memcpy(context + UCONTEXT_STACK_OFFSET, &stack_pointer, sizeof(stack_pointer));
    memcpy(context + UCONTEXT_STACK_OFFSET + 8, &stack_flags, sizeof(stack_flags));
    memcpy(context + UCONTEXT_STACK_OFFSET + 16, &stack_size, sizeof(stack_size));
}

static void read_user_context(struct syscall_frame *frame, const uint8_t *context) {
    arch_read_mcontext(frame, context);
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
    if (current->in_signal || !next_pending_signal(current)) {
        if (current->signal_wait_mask_active && !current->syscall_rewound)
            process_restore_signal_mask();
        return;
    }
    int signal_number = next_pending_signal(current);
    uint64_t bit = signal_bit(signal_number);
    __atomic_fetch_and(&current->signal_pending, ~bit, __ATOMIC_ACQ_REL);
    struct tunix_sigaction *action = &current->signal_actions[signal_number - 1];

    if (!signal_would_act(current, signal_number)) return;
    if (signal_number == SIGSTOP ||
        (action->handler == SIG_DFL &&
         (signal_number == SIGTSTP || signal_number == SIGTTIN ||
          signal_number == SIGTTOU))) {
        SCHED_LOCKED;
        wake_bump();
        current->stop_signal = signal_number;
        current->stop_reported = 0;
        current->continued_pending = 0;
        current->saved_frame = *frame;
        set_process_state(current, PROCESS_STOPPED);
        notify_parent_of_job_change(current);
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
        if (!current->syscall_force_restart &&
            (!(action->flags & SA_RESTART) || current->syscall_no_restart)) {
            SYSCALL_ADVANCE(frame);
            SYSCALL_RET(frame) = (uint64_t)-(int64_t)EINTR;
            current->io_wait_active = 0;
            current->io_wait_syscall = 0;
            current->io_wait_deadline_ns = 0;
        }
    }

    current->syscall_no_restart = 0;
    current->signal_waited = 0;
    uint64_t resume_mask = take_resume_mask();
    uint64_t stack_top = SYSCALL_USER_SP(frame);
    if ((action->flags & SA_ONSTACK) &&
        current->signal_stack_flags != SS_DISABLE &&
        !on_signal_stack(current, SYSCALL_USER_SP(frame))) {
        stack_top = current->signal_stack_pointer + current->signal_stack_size;
    }
    uint64_t area = stack_top & ~15ULL;
    uint64_t siginfo_address = 0;
    uint64_t context_address = 0;
    if (action->flags & SA_SIGINFO) {
        uint8_t context[SIGNAL_CONTEXT_SIZE];
        memset(context, 0, sizeof(context));
        fill_user_context(context, frame, resume_mask);
        area -= SIGNAL_CONTEXT_SIZE;
        context_address = area;
        if (copy_to_user(context_address, context, SIGNAL_CONTEXT_SIZE) != 0) {
            process_exit_from_signal(frame, SIGSEGV);
            return;
        }
        int32_t info[SIGNAL_SIGINFO_SIZE / 4];
        memset(info, 0, sizeof(info));
        info[0] = signal_number;
        int from_user = (current->signal_user_sent & bit) != 0;
        info[2] = from_user ? current->signal_sender_code[signal_number - 1] : SI_KERNEL;
        if (from_user) {
            info[4] = (int32_t)current->signal_sender_pid[signal_number - 1];
            info[5] = (int32_t)current->signal_sender_uid[signal_number - 1];
        }
        area -= SIGNAL_SIGINFO_SIZE;
        siginfo_address = area;
        if (copy_to_user(siginfo_address, info, SIGNAL_SIGINFO_SIZE) != 0) {
            process_exit_from_signal(frame, SIGSEGV);
            return;
        }
        area &= ~15ULL;
    }
    current->signal_user_sent &= ~bit;
    current->signal_sender_pid[signal_number - 1] = 0;
    current->signal_sender_uid[signal_number - 1] = 0;

    uint64_t new_rsp;
    process_memory_enter();
    int pushed = arch_signal_push_restorer(current->cr3, area, &action->restorer, &new_rsp);
    process_memory_leave();
    if (pushed != 0) {
        process_exit_from_signal(frame, SIGSEGV);
        return;
    }
    current->signal_saved_frame = *frame;
    current->signal_context_address = context_address;
    current->signal_saved_mask = resume_mask;
    current->signal_blocked |= action->mask | bit;
    current->in_signal = 1;
    arch_signal_enter_handler(frame, new_rsp, action->handler, action->restorer,
                              signal_number, siginfo_address, context_address);
}

void process_set_fs_base(uint64_t value) {
    if (!current || value >= USER_ADDRESS_LIMIT) return;
    current->fs_base = value;
    arch_write_fs_base(value);
}

uint64_t process_get_fs_base(void) {
    return current ? current->fs_base : 0;
}

void process_set_gs_base(uint64_t value) {
    if (!current) return;
    current->gs_base = value;
    arch_write_gs_base(value);
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
    SCHED_LOCKED;
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
    SCHED_LOCKED;
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
    SCHED_LOCKED;
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
    SCHED_LOCKED;
    if (!queue) return 0;
    uint64_t total = 0;
    struct process *item = queue;
    do {
        if (item->state != PROCESS_DEAD) total += process_runtime_ns(item);
        item = item->next;
    } while (item != queue);
    return total;
}
