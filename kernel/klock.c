#include <stdint.h>

#include "include/cpu.h"
#include "include/klock.h"
#include "include/percpu.h"
#include "include/smp.h"
#include "include/time.h"

extern void kprintf(const char *fmt, ...);

static volatile uint32_t next_ticket;
static volatile uint32_t now_serving;

#define KLOCK_MODE_NONE 0
#define KLOCK_MODE_EXCLUSIVE 1
#define KLOCK_MODE_SHARED 2

/* Every shared entry writes only its processor's cache line. A single global
   reader count would still serialize the readers in the cache-coherency
   protocol even though the lock admitted them together. */
struct klock_cpu_state {
    volatile uint32_t shared_holders;
    volatile uint8_t held_mode;
    volatile uint8_t watchdog_reported;
    volatile uint8_t taken_by_isr;
    volatile uint8_t isr_depth;
    volatile uint32_t breadcrumb;
    uint8_t padding[52];
} __attribute__((aligned(64)));

_Static_assert(sizeof(struct klock_cpu_state) == 64,
               "one klock state must occupy one cache line");

static struct klock_cpu_state cpu_state[SMP_MAX_CPUS];

#define KLOCK_WATCHDOG_NS (20ULL * 1000ULL * 1000ULL * 1000ULL)

void klock_note(uint32_t what) {
    cpu_state[cpu_current()->index].breadcrumb = what;
}

static int klock_stats_on;
static uint64_t hold_started;
static struct klock_hold holds[KLOCK_HOLD_SLOTS];

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

int klock_statistics(unsigned index, struct klock_hold *out) {
    if (!out || index >= KLOCK_HOLD_SLOTS || !holds[index].count) return -1;
    *out = holds[index];
    uint64_t hz = time_tsc_frequency();
    if (!hz) return 0;
    out->total_ns = out->total_ns / (hz / 1000000ULL) * 1000ULL;
    out->max_ns = out->max_ns / (hz / 1000000ULL) * 1000ULL;
    return 0;
}

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
    hold_started = cpu_counter();
}

static void hold_end(void) {
    if (!hold_started) return;
    uint64_t now = cpu_counter();
    uint64_t held = now > hold_started ? now - hold_started : 0;
    hold_started = 0;
    record_hold(cpu_state[cpu_current()->index].breadcrumb, held);
}

static uint32_t shared_total(void) {
    uint32_t total = 0;
    for (unsigned index = 0; index < SMP_MAX_CPUS; index++)
        total += __atomic_load_n(&cpu_state[index].shared_holders,
                                 __ATOMIC_SEQ_CST);
    return total;
}

static void klock_report(const char *what, uint32_t ticket) {
    unsigned self = cpu_current()->index;
    if (cpu_state[self].watchdog_reported) return;
    cpu_state[self].watchdog_reported = 1;
    kprintf("KLOCK: cpu %u stuck %s %u: next %u serving %u shared %d\n",
            cpu_current()->index, what, (unsigned)ticket,
            (unsigned)__atomic_load_n(&next_ticket, __ATOMIC_RELAXED),
            (unsigned)__atomic_load_n(&now_serving, __ATOMIC_RELAXED),
            (int)shared_total());
    for (unsigned index = 0; index < SMP_MAX_CPUS; index++) {
        struct cpu *cpu = percpu_slot(index);
        if (!cpu || !cpu->online) continue;
        kprintf("KLOCK: cpu %u holds %u doing %x\n", index,
                (unsigned)cpu_state[index].held_mode,
                (unsigned)cpu_state[index].breadcrumb);
    }
}

struct klock_watchdog {
    uint64_t deadline;
    unsigned spins;
    int reported;
};

static int watchdog_expired(struct klock_watchdog *watchdog) {
    if (watchdog->reported || (++watchdog->spins & 1023U)) return 0;
    uint64_t now = time_uptime_ns();
    if (!watchdog->deadline) {
        watchdog->deadline = now + KLOCK_WATCHDOG_NS;
        return 0;
    }
    if (now < watchdog->deadline) return 0;
    watchdog->reported = 1;
    return 1;
}

static void wait_for_turn(uint32_t ticket) {
    struct klock_watchdog watchdog = {0, 0, 0};
    while (__atomic_load_n(&now_serving, __ATOMIC_ACQUIRE) != ticket) {
        smp_service_flush();
        cpu_relax();
        if (watchdog_expired(&watchdog)) klock_report("waiting for ticket", ticket);
    }
}

void kernel_lock(void) {
    uint32_t ticket = __atomic_fetch_add(&next_ticket, 1, __ATOMIC_RELAXED);
    wait_for_turn(ticket);
    struct klock_watchdog watchdog = {0, 0, 0};
    while (shared_total() != 0) {
        smp_service_flush();
        cpu_relax();
        if (watchdog_expired(&watchdog))
            klock_report("waiting for shared holders to leave", ticket);
    }
    cpu_state[cpu_current()->index].held_mode = KLOCK_MODE_EXCLUSIVE;
    hold_begin();
}

int kernel_lock_release_for_wait(void) {
    if (kernel_lock_in_interrupt()) return 0;
    if (cpu_state[cpu_current()->index].held_mode != KLOCK_MODE_EXCLUSIVE) return 0;
    kernel_unlock();
    return 1;
}

void kernel_lock_retake_after_wait(int released) {
    if (!released) return;
    kernel_lock();
}

void kernel_lock_wait_tick(void) {
    smp_service_flush();
}

void kernel_unlock(void) {
    hold_end();
    cpu_state[cpu_current()->index].held_mode = KLOCK_MODE_NONE;
    __atomic_store_n(&now_serving, now_serving + 1, __ATOMIC_RELEASE);
}

void kernel_lock_shared(void) {
    struct klock_cpu_state *state = &cpu_state[cpu_current()->index];
    __atomic_fetch_add(&state->shared_holders, 1, __ATOMIC_SEQ_CST);
    if (__atomic_load_n(&next_ticket, __ATOMIC_SEQ_CST) !=
        __atomic_load_n(&now_serving, __ATOMIC_SEQ_CST)) {
        __atomic_fetch_sub(&state->shared_holders, 1, __ATOMIC_SEQ_CST);
        uint32_t ticket = __atomic_fetch_add(&next_ticket, 1, __ATOMIC_SEQ_CST);
        wait_for_turn(ticket);
        __atomic_fetch_add(&state->shared_holders, 1, __ATOMIC_SEQ_CST);
        __atomic_store_n(&now_serving, now_serving + 1, __ATOMIC_RELEASE);
    }
    state->held_mode = KLOCK_MODE_SHARED;
}

void kernel_unlock_shared(void) {
    struct klock_cpu_state *state = &cpu_state[cpu_current()->index];
    state->held_mode = KLOCK_MODE_NONE;
    __atomic_fetch_sub(&state->shared_holders, 1, __ATOMIC_SEQ_CST);
}

#define ISR_LOCK_NOTHING 0U
#define ISR_LOCK_TAKEN   1U
#define ISR_LOCK_SHARED  2U

static volatile uint32_t isr_holder;

int kernel_lock_in_interrupt(void) {
    return cpu_state[cpu_current()->index].isr_depth != 0;
}

void kernel_lock_from_isr(void) {
    unsigned index = cpu_current()->index;
    struct klock_cpu_state *state = &cpu_state[index];
    state->isr_depth++;
    if (kernel_lock_shared_here()) {
        uint32_t me = index + 1U;
        if (__atomic_load_n(&isr_holder, __ATOMIC_RELAXED) == me) {
            state->taken_by_isr = ISR_LOCK_NOTHING;
            return;
        }
        for (;;) {
            uint32_t nobody = 0;
            if (__atomic_compare_exchange_n(&isr_holder, &nobody, me, 0,
                                            __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
                break;
            cpu_relax();
        }
        state->taken_by_isr = ISR_LOCK_SHARED;
        return;
    }
    if (kernel_lock_held_here()) {
        state->taken_by_isr = ISR_LOCK_NOTHING;
        return;
    }
    kernel_lock();
    state->taken_by_isr = ISR_LOCK_TAKEN;
}

void kernel_unlock_from_isr(void) {
    unsigned index = cpu_current()->index;
    struct klock_cpu_state *cpu = &cpu_state[index];
    if (cpu->isr_depth) cpu->isr_depth--;
    uint8_t state = cpu->taken_by_isr;
    cpu->taken_by_isr = ISR_LOCK_NOTHING;
    if (state == ISR_LOCK_TAKEN) kernel_unlock_current();
    else if (state == ISR_LOCK_SHARED)
        __atomic_store_n(&isr_holder, 0U, __ATOMIC_RELEASE);
}

int kernel_lock_held_here(void) {
    return cpu_state[cpu_current()->index].held_mode != KLOCK_MODE_NONE;
}

int kernel_lock_shared_here(void) {
    return cpu_state[cpu_current()->index].held_mode == KLOCK_MODE_SHARED;
}

void kernel_unlock_current(void) {
    if (cpu_state[cpu_current()->index].held_mode == KLOCK_MODE_SHARED)
        kernel_unlock_shared();
    else kernel_unlock();
}
