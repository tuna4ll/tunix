#include <stdint.h>

#include "include/cpu.h"
#include "include/klock.h"
#include "include/percpu.h"
#include "include/smp.h"
#include "include/time.h"

extern void kprintf(const char *fmt, ...);

static volatile uint32_t next_ticket;
static volatile uint32_t now_serving;
static volatile int32_t shared_holders;

#define KLOCK_MODE_NONE 0
#define KLOCK_MODE_EXCLUSIVE 1
#define KLOCK_MODE_SHARED 2

static volatile uint8_t held_mode[SMP_MAX_CPUS];

#define KLOCK_WATCHDOG_NS (20ULL * 1000ULL * 1000ULL * 1000ULL)

static volatile uint32_t breadcrumb[SMP_MAX_CPUS];

void klock_note(uint32_t what) {
    breadcrumb[cpu_current()->index] = what;
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
    record_hold(breadcrumb[cpu_current()->index], held);
}

static volatile uint8_t watchdog_reported[SMP_MAX_CPUS];

static void klock_report(const char *what, uint32_t ticket) {
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
        cpu_relax();
        if (!reported && time_uptime_ns() >= deadline) {
            reported = 1;
            klock_report("waiting for ticket", ticket);
        }
    }
}

void kernel_lock(void) {
    uint32_t ticket = __atomic_fetch_add(&next_ticket, 1, __ATOMIC_RELAXED);
    wait_for_turn(ticket);
    uint64_t deadline = time_uptime_ns() + KLOCK_WATCHDOG_NS;
    int reported = 0;
    while (__atomic_load_n(&shared_holders, __ATOMIC_ACQUIRE) != 0) {
        smp_service_flush();
        cpu_relax();
        if (!reported && time_uptime_ns() >= deadline) {
            reported = 1;
            klock_report("waiting for shared holders to leave", ticket);
        }
    }
    held_mode[cpu_current()->index] = KLOCK_MODE_EXCLUSIVE;
    hold_begin();
}

int kernel_lock_release_for_wait(void) {
    if (kernel_lock_in_interrupt()) return 0;
    if (held_mode[cpu_current()->index] != KLOCK_MODE_EXCLUSIVE) return 0;
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
    held_mode[cpu_current()->index] = KLOCK_MODE_NONE;
    __atomic_store_n(&now_serving, now_serving + 1, __ATOMIC_RELEASE);
}

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

#define ISR_LOCK_NOTHING 0U
#define ISR_LOCK_TAKEN   1U
#define ISR_LOCK_SHARED  2U

static volatile uint8_t taken_by_isr[SMP_MAX_CPUS];
static volatile uint32_t isr_holder;
static volatile uint8_t isr_depth[SMP_MAX_CPUS];

int kernel_lock_in_interrupt(void) {
    return isr_depth[cpu_current()->index] != 0;
}

void kernel_lock_from_isr(void) {
    unsigned index = cpu_current()->index;
    isr_depth[index]++;
    if (kernel_lock_shared_here()) {
        uint32_t me = index + 1U;
        if (__atomic_load_n(&isr_holder, __ATOMIC_RELAXED) == me) {
            taken_by_isr[index] = ISR_LOCK_NOTHING;
            return;
        }
        for (;;) {
            uint32_t nobody = 0;
            if (__atomic_compare_exchange_n(&isr_holder, &nobody, me, 0,
                                            __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
                break;
            cpu_relax();
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
    if (isr_depth[index]) isr_depth[index]--;
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

void kernel_unlock_current(void) {
    if (held_mode[cpu_current()->index] == KLOCK_MODE_SHARED) kernel_unlock_shared();
    else kernel_unlock();
}
