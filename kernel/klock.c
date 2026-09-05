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

/*
 * A breadcrumb per processor: what it was last doing that mattered.
 *
 * The watchdog can say a processor is holding the lock and not giving it back,
 * which is half a diagnosis. The half that matters is what it is holding it
 * for, and there is no stack to walk from another processor. So the few places
 * that take the lock leave a number behind, and the report prints it.
 */
static volatile uint32_t breadcrumb[SMP_MAX_CPUS];

void klock_note(uint32_t what) {
    breadcrumb[cpu_current()->index] = what;
}

/*
 * Where the waiting time goes.
 *
 * The watchdog only speaks after twenty seconds, and the holds that matter are
 * far shorter than that and far more frequent: an input event is only noticed
 * when the tick or the keyboard interrupt can take the lock, so a hold of a few
 * hundred milliseconds is a key that reaches a compositor late enough for its
 * own repeat to fire. Measured on real hardware: a press and its release
 * arrived 433 ms apart under weston against about 100 ms at a terminal.
 *
 * So each exclusive hold is timed and filed under the breadcrumb of whoever
 * took it, and /proc/klock reports the worst. Writing to that file is what
 * starts the recording: measured, always-on costs a fifth of the cheapest
 * syscall, and this is a question that is only ever asked deliberately.
 */
static int klock_stats_on;
static uint64_t hold_started;
static uint32_t hold_note;
static struct klock_hold holds[KLOCK_HOLD_SLOTS];

/* Turned on from userland -- writing to /proc/klock -- rather than at boot,
   because two reads of the cycle counter on every syscall is a fifth of what
   the cheapest one costs, and nobody should pay that until they are asking. */
void klock_statistics_stop(void) {
    __atomic_store_n(&klock_stats_on, 0, __ATOMIC_RELEASE);
}

void klock_statistics_start(void) {
    for (unsigned index = 0; index < KLOCK_HOLD_SLOTS; index++) {
        holds[index].note = 0;
        holds[index].count = 0;
        holds[index].total_ns = 0;
        holds[index].max_ns = 0;
    }
    hold_started = 0;
    __atomic_store_n(&klock_stats_on, 1, __ATOMIC_RELEASE);
}

/* The counter itself, not a converted time: two of these is what the
   measurement costs every syscall, and the arithmetic can wait until somebody
   reads the report. */
static inline uint64_t read_tsc(void) {
    uint32_t low, high;
    __asm__ volatile("rdtsc" : "=a"(low), "=d"(high));
    return ((uint64_t)high << 32) | low;
}

int klock_statistics(unsigned index, struct klock_hold *out) {
    if (!out || index >= KLOCK_HOLD_SLOTS || !holds[index].count) return -1;
    *out = holds[index];
    uint64_t hz = time_tsc_frequency();
    if (!hz) return 0;
    out->total_ns = out->total_ns / (hz / 1000000ULL) * 1000ULL;
    out->max_ns = out->max_ns / (hz / 1000000ULL) * 1000ULL;
    return 0;
}

/* Filed under the breadcrumb, with the slowest kept when the table is full:
   what is worth reporting is the hold that made somebody wait. */
static void record_hold(uint32_t note, uint64_t nanoseconds) {
    unsigned free_slot = KLOCK_HOLD_SLOTS;
    unsigned weakest = 0;
    for (unsigned index = 0; index < KLOCK_HOLD_SLOTS; index++) {
        if (holds[index].count && holds[index].note == note) {
            holds[index].count++;
            holds[index].total_ns += nanoseconds;
            if (nanoseconds > holds[index].max_ns) holds[index].max_ns = nanoseconds;
            return;
        }
        if (!holds[index].count && free_slot == KLOCK_HOLD_SLOTS) free_slot = index;
        if (holds[index].max_ns < holds[weakest].max_ns) weakest = index;
    }
    unsigned slot = free_slot < KLOCK_HOLD_SLOTS ? free_slot : weakest;
    if (free_slot == KLOCK_HOLD_SLOTS && nanoseconds <= holds[slot].max_ns) return;
    holds[slot].note = note;
    holds[slot].count = 1;
    holds[slot].total_ns = nanoseconds;
    holds[slot].max_ns = nanoseconds;
}

static void hold_begin(void) {
    if (!__atomic_load_n(&klock_stats_on, __ATOMIC_RELAXED)) return;
    hold_note = breadcrumb[cpu_current()->index];
    hold_started = read_tsc();
}

static void hold_end(void) {
    if (!hold_started) return;
    uint64_t now = read_tsc();
    uint64_t held = now > hold_started ? now - hold_started : 0;
    hold_started = 0;
    record_hold(hold_note, held);
}

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
        kprintf("KLOCK: cpu %u holds %u doing %x\n", index,
                (unsigned)held_mode[index], (unsigned)breadcrumb[index]);
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
    hold_begin();
}

void kernel_unlock(void) {
    hold_end();
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
/* What the handler did to the lock on the way in, so the way out can undo it. */
#define ISR_LOCK_NOTHING 0U
#define ISR_LOCK_TAKEN   1U
#define ISR_LOCK_SHARED  2U

static volatile uint8_t taken_by_isr[SMP_MAX_CPUS];
/* Processor index plus one, zero for nobody. */
static volatile uint32_t isr_holder;

void kernel_lock_from_isr(void) {
    unsigned index = cpu_current()->index;
    /*
     * A shared holder excludes nobody, so an interrupt that landed on one used
     * to be let through here as if it already had the lock -- and then ran
     * beside another processor's handler doing the same thing. Both walk the
     * run queue, drain the keyboard controller and dispatch driver interrupts,
     * none of which is written to be entered twice at once.
     *
     * Upgrading is not the answer: an exclusive ticket waits for the shared
     * holders inside to leave, and this processor is one of them, so it would
     * wait for itself. What the handler gets instead is exclusion against the
     * one thing its shared claim does not already give it -- another
     * processor's handler. Nothing exclusive can be inside while either of us
     * is, because it would have waited for our shared claims to go.
     */
    if (kernel_lock_shared_here()) {
        uint32_t me = index + 1U;
        /* A fault taken inside a handler, which panics; do not wait for a
           ticket this processor is already holding. */
        if (__atomic_load_n(&isr_holder, __ATOMIC_RELAXED) == me) {
            taken_by_isr[index] = ISR_LOCK_NOTHING;
            return;
        }
        for (;;) {
            uint32_t nobody = 0;
            if (__atomic_compare_exchange_n(&isr_holder, &nobody, me, 0,
                                            __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
                break;
            __asm__ volatile("pause");
        }
        taken_by_isr[index] = ISR_LOCK_SHARED;
        return;
    }
    if (kernel_lock_held_here()) {
        taken_by_isr[index] = ISR_LOCK_NOTHING;
        return;
    }
    kernel_lock();
    taken_by_isr[index] = ISR_LOCK_TAKEN;
}

void kernel_unlock_from_isr(void) {
    unsigned index = cpu_current()->index;
    uint8_t state = taken_by_isr[index];
    taken_by_isr[index] = ISR_LOCK_NOTHING;
    if (state == ISR_LOCK_TAKEN) kernel_unlock_current();
    else if (state == ISR_LOCK_SHARED)
        __atomic_store_n(&isr_holder, 0U, __ATOMIC_RELEASE);
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
