#include <stdint.h>

#include "include/klock.h"
#include "include/percpu.h"
#include "include/smp.h"
#include "include/time.h"

extern void kprintf(const char *fmt, ...);

/* See include/klock.h for what the two modes are and what was measured. */

static volatile uint32_t next_ticket;
static volatile uint32_t now_serving;
/* How many processors are inside in shared mode. An exclusive holder waits for
   this to reach zero before it starts, which is what makes the two exclusive
   of one another. */
static volatile int32_t shared_holders;

#define KLOCK_MODE_NONE 0
#define KLOCK_MODE_EXCLUSIVE 1
#define KLOCK_MODE_SHARED 2

/* What each processor is holding, indexed by processor. Only the processor
   itself writes its own entry, so no lock protects this. */
static volatile uint8_t held_mode[SMP_MAX_CPUS];

/*
 * Wait for this ticket to come up. The flush service runs inside the wait
 * because interrupts are off: a processor asking this one to drop cached
 * translations cannot arrive as an interrupt, and the processor doing the
 * asking may be the very one holding the lock this is waiting for.
 */
/*
 * A wait this long is not contention, it is a lock that will never come.
 *
 * Both ways that happens are silent from outside: an unlock with nothing held
 * moves the queue past a ticket nobody was serving, and a shared holder that
 * left without decrementing keeps every exclusive waiter out for good. Either
 * way the machine stops on somebody's next syscall having printed nothing at
 * all, which is a diagnosis nobody can make. So it says what it is waiting for
 * and what the lock looks like, once, and goes on waiting.
 */
/* Twenty seconds, not five. The lock is held across block reads, and a root
   filesystem on a USB stick makes some of those genuinely slow -- five caught
   weston loading itself, which is not the thing worth reporting. Nothing that
   is going to finish takes twenty. */
#define KLOCK_WATCHDOG_NS (20ULL * 1000ULL * 1000ULL * 1000ULL)

static volatile uint8_t watchdog_reported[SMP_MAX_CPUS];

static void klock_report(const char *what, uint32_t ticket) {
    /* Once per processor. A lock that is being waited on this long is being
       waited on by everybody, and the point is to be readable. */
    unsigned self = cpu_current()->index;
    if (watchdog_reported[self]) return;
    watchdog_reported[self] = 1;
    kprintf("KLOCK: cpu %u stuck %s %u: next %u serving %u shared %d\n",
            cpu_current()->index, what, (unsigned)ticket,
            (unsigned)__atomic_load_n(&next_ticket, __ATOMIC_RELAXED),
            (unsigned)__atomic_load_n(&now_serving, __ATOMIC_RELAXED),
            (int)__atomic_load_n(&shared_holders, __ATOMIC_RELAXED));
    for (unsigned index = 0; index < SMP_MAX_CPUS; index++) {
        struct cpu *cpu = percpu_slot(index);
        if (!cpu || !cpu->online) continue;
        kprintf("KLOCK: cpu %u holds %u\n", index, (unsigned)held_mode[index]);
    }
}

static void wait_for_turn(uint32_t ticket) {
    uint64_t deadline = time_uptime_ns() + KLOCK_WATCHDOG_NS;
    int reported = 0;
    while (__atomic_load_n(&now_serving, __ATOMIC_ACQUIRE) != ticket) {
        smp_service_flush();
        __asm__ volatile("pause");
        if (!reported && time_uptime_ns() >= deadline) {
            reported = 1;
            klock_report("waiting for ticket", ticket);
        }
    }
}

void kernel_lock(void) {
    uint32_t ticket = __atomic_fetch_add(&next_ticket, 1, __ATOMIC_RELAXED);
    wait_for_turn(ticket);
    /* The ticket is ours, but shared holders admitted before it may still be
       inside. They cannot grow in number -- their tickets are behind ours. */
    uint64_t deadline = time_uptime_ns() + KLOCK_WATCHDOG_NS;
    int reported = 0;
    while (__atomic_load_n(&shared_holders, __ATOMIC_ACQUIRE) != 0) {
        smp_service_flush();
        __asm__ volatile("pause");
        if (!reported && time_uptime_ns() >= deadline) {
            reported = 1;
            klock_report("waiting for shared holders to leave", ticket);
        }
    }
    held_mode[cpu_current()->index] = KLOCK_MODE_EXCLUSIVE;
}

void kernel_unlock(void) {
    held_mode[cpu_current()->index] = KLOCK_MODE_NONE;
    __atomic_store_n(&now_serving, now_serving + 1, __ATOMIC_RELEASE);
}

/*
 * Shared entry, without taking a ticket when there is nobody to queue behind.
 *
 * Taking one costs three read-modify-writes on two shared words for every
 * syscall, and the processors then hand the ticket round one at a time -- which
 * showed up as 17% of all samples once the paths that matter had been moved
 * here. So the common case is a single increment and two loads: announce
 * ourselves, then check that no exclusive holder was already inside or queued.
 *
 * The check has to come after the increment, not before. An exclusive holder
 * waits for this counter to reach zero, so a processor that announced itself
 * first is one the exclusive holder will wait for; one that looked first and
 * announced afterwards could slip in behind its back.
 *
 * If an exclusive holder is queued we stand down and take a ticket after all,
 * which is what keeps it from being starved by a stream of shared arrivals.
 */
void kernel_lock_shared(void) {
    __atomic_fetch_add(&shared_holders, 1, __ATOMIC_ACQUIRE);
    if (__atomic_load_n(&next_ticket, __ATOMIC_ACQUIRE) !=
        __atomic_load_n(&now_serving, __ATOMIC_ACQUIRE)) {
        __atomic_fetch_sub(&shared_holders, 1, __ATOMIC_RELEASE);
        uint32_t ticket = __atomic_fetch_add(&next_ticket, 1, __ATOMIC_RELAXED);
        wait_for_turn(ticket);
        __atomic_fetch_add(&shared_holders, 1, __ATOMIC_ACQUIRE);
        __atomic_store_n(&now_serving, now_serving + 1, __ATOMIC_RELEASE);
    }
    held_mode[cpu_current()->index] = KLOCK_MODE_SHARED;
}

void kernel_unlock_shared(void) {
    held_mode[cpu_current()->index] = KLOCK_MODE_NONE;
    __atomic_fetch_sub(&shared_holders, 1, __ATOMIC_RELEASE);
}

/*
 * The lock, taken by an interrupt that may have landed on a processor already
 * holding it.
 *
 * The two halves have to agree. The handler used to take it only when it was
 * free while the entry stub released it unconditionally on the way out, so an
 * interrupt arriving on a processor that was already inside the kernel handed
 * back a lock it never took: the ticket queue moved past a ticket nobody was
 * serving, and every later attempt to take the lock waited for a turn that had
 * already gone by. What that looks like is a machine that stops on its next
 * syscall having printed nothing.
 */
static volatile uint8_t taken_by_isr[SMP_MAX_CPUS];

void kernel_lock_from_isr(void) {
    unsigned index = cpu_current()->index;
    if (kernel_lock_held_here()) {
        taken_by_isr[index] = 0;
        return;
    }
    kernel_lock();
    taken_by_isr[index] = 1;
}

void kernel_unlock_from_isr(void) {
    unsigned index = cpu_current()->index;
    if (!taken_by_isr[index]) return;
    taken_by_isr[index] = 0;
    kernel_unlock_current();
}

int kernel_lock_held_here(void) {
    return held_mode[cpu_current()->index] != KLOCK_MODE_NONE;
}

int kernel_lock_shared_here(void) {
    return held_mode[cpu_current()->index] == KLOCK_MODE_SHARED;
}

/*
 * Release whichever mode this processor took.
 *
 * The entry stubs call this by name on the way out and have no way to know
 * which of the two the dispatcher chose -- that decision is made per syscall,
 * in C, after the stub has already been entered. Keeping the choice in one
 * place here is what lets the assembly stay a single unconditional call.
 */
void kernel_unlock_current(void) {
    if (held_mode[cpu_current()->index] == KLOCK_MODE_SHARED) kernel_unlock_shared();
    else kernel_unlock();
}
