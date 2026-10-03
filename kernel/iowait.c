#include <stddef.h>
#include <stdint.h>

#include "include/cpu.h"
#include "include/iowait.h"
#include "include/lock.h"
#include "include/percpu.h"
#include "include/process.h"
#include "include/smp.h"
#include "include/time.h"

#define IO_SPIN_NS 500000ULL
#define IO_NAP_NS 1000000ULL

static const char tick_channel;
static volatile uint32_t sleepers;

static int can_sleep_holding(struct lock *held) {
    return process_current() && !cpu_current()->in_interrupt && lock_only_holds(held);
}

int io_poll_dropping(io_ready_fn ready, void *context, uint64_t timeout_ns,
                     struct lock *held, const void *channel) {
    uint64_t started = time_uptime_ns();
    uint64_t deadline = started + timeout_ns;
    if (ready(context)) return 0;
    while (time_uptime_ns() - started < IO_SPIN_NS) {
        if (ready(context)) return 0;
        smp_service_flush();
        cpu_relax();
    }
    const void *wake = channel ? channel : &tick_channel;
    for (;;) {
        if (ready(context)) return 0;
        uint64_t now = time_uptime_ns();
        if (now >= deadline) return ready(context) ? 0 : -1;
        if (!can_sleep_holding(held)) {
            smp_service_flush();
            cpu_relax();
            continue;
        }
        uint64_t nap = now + IO_NAP_NS < deadline ? now + IO_NAP_NS : deadline;
        __atomic_fetch_add(&sleepers, 1, __ATOMIC_ACQ_REL);
        process_prepare_wait(wake, nap);
        if (ready(context)) {
            process_finish_wait();
            __atomic_fetch_sub(&sleepers, 1, __ATOMIC_ACQ_REL);
            return 0;
        }
        if (held) lock_release(held);
        process_wait();
        process_finish_wait();
        if (held) lock_acquire(held);
        __atomic_fetch_sub(&sleepers, 1, __ATOMIC_ACQ_REL);
    }
}

void io_nap(struct lock *held) {
    if (!can_sleep_holding(held)) {
        smp_service_flush();
        cpu_relax();
        return;
    }
    __atomic_fetch_add(&sleepers, 1, __ATOMIC_ACQ_REL);
    process_prepare_wait(&tick_channel, time_uptime_ns() + IO_NAP_NS);
    if (held) lock_release(held);
    process_wait();
    process_finish_wait();
    if (held) lock_acquire(held);
    __atomic_fetch_sub(&sleepers, 1, __ATOMIC_ACQ_REL);
}

int io_poll(io_ready_fn ready, void *context, uint64_t timeout_ns) {
    return io_poll_dropping(ready, context, timeout_ns, NULL, NULL);
}

void io_poll_tick(void) {
    if (__atomic_load_n(&sleepers, __ATOMIC_ACQUIRE)) process_wake_all(&tick_channel);
}
