#include <stdint.h>

#include <tunix/cpu.h>
#include <tunix/lock.h>
#include <tunix/percpu.h>
#include <tunix/smp.h>
#include <tunix/time.h>

extern void kprintf(const char *fmt, ...);

#define HELD_MAX 24U
#define ORDER_REPORTS 16U
#define LOCK_WATCHDOG_NS (20ULL * 1000ULL * 1000ULL * 1000ULL)

struct held_locks {
    struct lock *locks[HELD_MAX];
    uint32_t count;
    uint32_t reported;
} __attribute__((aligned(64)));

static struct held_locks held[SMP_MAX_CPUS];
static volatile uint32_t order_reports;

void lock_init(struct lock *lock, const char *name, unsigned rank) {
    lock->next = 0;
    lock->serving = 0;
    lock->owner = 0;
    lock->depth = 0;
    lock->rank = rank;
    lock->name = name;
}

static uint32_t self_id(void) {
    return cpu_current()->index + 1U;
}

static void check_order(struct held_locks *mine, const struct lock *lock) {
    for (uint32_t index = 0; index < mine->count; index++) {
        const struct lock *other = mine->locks[index];
        if (other->rank <= lock->rank) continue;
        if (__atomic_fetch_add(&order_reports, 1, __ATOMIC_RELAXED) >= ORDER_REPORTS) return;
        kprintf("LOCK: cpu %u takes %s (rank %u) while holding %s (rank %u)\n",
                cpu_current()->index, lock->name, lock->rank, other->name, other->rank);
        return;
    }
}

static void remember(struct held_locks *mine, struct lock *lock) {
    if (mine->count < HELD_MAX) mine->locks[mine->count] = lock;
    mine->count++;
}

static void forget(struct held_locks *mine, struct lock *lock) {
    if (!mine->count) return;
    uint32_t top = mine->count < HELD_MAX ? mine->count : HELD_MAX;
    for (uint32_t index = top; index-- > 0;) {
        if (mine->locks[index] != lock) continue;
        for (uint32_t move = index; move + 1U < top; move++)
            mine->locks[move] = mine->locks[move + 1U];
        break;
    }
    mine->count--;
}

static void report_stuck(const struct lock *lock, uint32_t ticket) {
    uint32_t owner = __atomic_load_n(&lock->owner, __ATOMIC_RELAXED);
    kprintf("LOCK: cpu %u stuck on %s ticket %u, serving %u, held by cpu %d\n",
            cpu_current()->index, lock->name, (unsigned)ticket,
            (unsigned)__atomic_load_n(&lock->serving, __ATOMIC_RELAXED),
            (int)owner - 1);
    if (!owner || owner > SMP_MAX_CPUS) return;
    struct held_locks *theirs = &held[owner - 1U];
    uint32_t top = theirs->count < HELD_MAX ? theirs->count : HELD_MAX;
    for (uint32_t index = 0; index < top; index++)
        kprintf("LOCK:   cpu %d holds %s\n", (int)owner - 1, theirs->locks[index]->name);
}

void lock_acquire(struct lock *lock) {
    uint32_t me = self_id();
    struct held_locks *mine = &held[me - 1U];
    if (__atomic_load_n(&lock->owner, __ATOMIC_RELAXED) == me) {
        lock->depth++;
        return;
    }
    check_order(mine, lock);
    uint32_t ticket = __atomic_fetch_add(&lock->next, 1, __ATOMIC_RELAXED);
    uint64_t deadline = 0;
    unsigned spins = 0;
    int reported = 0;
    while (__atomic_load_n(&lock->serving, __ATOMIC_ACQUIRE) != ticket) {
        smp_service_flush();
        cpu_relax();
        if (reported || (++spins & 4095U)) continue;
        uint64_t now = time_uptime_ns();
        if (!deadline) deadline = now + LOCK_WATCHDOG_NS;
        else if (now >= deadline) {
            reported = 1;
            report_stuck(lock, ticket);
        }
    }
    __atomic_store_n(&lock->owner, me, __ATOMIC_RELAXED);
    lock->depth = 1;
    remember(mine, lock);
}

int lock_try_acquire(struct lock *lock) {
    uint32_t me = self_id();
    if (__atomic_load_n(&lock->owner, __ATOMIC_RELAXED) == me) {
        lock->depth++;
        return 1;
    }
    uint32_t serving = __atomic_load_n(&lock->serving, __ATOMIC_ACQUIRE);
    uint32_t expected = serving;
    if (!__atomic_compare_exchange_n(&lock->next, &expected, serving + 1U, 0,
                                     __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
        return 0;
    __atomic_store_n(&lock->owner, me, __ATOMIC_RELAXED);
    lock->depth = 1;
    remember(&held[me - 1U], lock);
    return 1;
}

void lock_release(struct lock *lock) {
    uint32_t me = self_id();
    if (__atomic_load_n(&lock->owner, __ATOMIC_RELAXED) != me) {
        kprintf("LOCK: cpu %u releases %s it does not hold\n", me - 1U, lock->name);
        return;
    }
    if (--lock->depth) return;
    forget(&held[me - 1U], lock);
    __atomic_store_n(&lock->owner, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&lock->serving, lock->serving + 1U, __ATOMIC_RELEASE);
}

int lock_held(const struct lock *lock) {
    return __atomic_load_n(&lock->owner, __ATOMIC_RELAXED) == self_id();
}

unsigned lock_depth_here(void) {
    return held[cpu_current()->index].count;
}

void lock_check_released(const char *where) {
    struct held_locks *mine = &held[cpu_current()->index];
    if (!mine->count) return;
    if (!mine->reported && __atomic_fetch_add(&order_reports, 1, __ATOMIC_RELAXED) < ORDER_REPORTS) {
        mine->reported = 1;
        kprintf("LOCK: cpu %u leaves %s holding %u lock(s), first %s\n",
                cpu_current()->index, where, (unsigned)mine->count,
                mine->locks[0]->name);
    }
}

unsigned lock_depth(const struct lock *lock) {
    return lock_held(lock) ? lock->depth : 0;
}

void lock_set_depth(struct lock *lock, unsigned depth) {
    if (lock_held(lock) && depth) lock->depth = depth;
}

int lock_only_holds(const struct lock *lock) {
    struct held_locks *mine = &held[cpu_current()->index];
    if (!lock) return mine->count == 0;
    return mine->count == 1 && mine->locks[0] == lock;
}

void lock_report_sleep(const char *what) {
    struct held_locks *mine = &held[cpu_current()->index];
    if (__atomic_fetch_add(&order_reports, 1, __ATOMIC_RELAXED) >= ORDER_REPORTS) return;
    kprintf("LOCK: cpu %u sleeps in %s holding %u lock(s), first %s\n",
            cpu_current()->index, what, (unsigned)mine->count,
            mine->count ? mine->locks[0]->name : "none");
}

void lock_drop(struct lock *lock) {
    if (!lock_held(lock)) return;
    lock->depth = 1;
    lock_release(lock);
}
