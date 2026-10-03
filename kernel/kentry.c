#include <stdint.h>

#include "include/defer.h"
#include "include/kentry.h"
#include "include/process.h"

static int overlap_tracking;
static volatile uint32_t overlap_peak;

void kernel_overlap_start(void) {
    __atomic_store_n(&overlap_peak, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&overlap_tracking, 1, __ATOMIC_RELEASE);
}

void kernel_overlap_stop(void) {
    __atomic_store_n(&overlap_tracking, 0, __ATOMIC_RELEASE);
}

unsigned kernel_overlap_peak(void) {
    return __atomic_load_n(&overlap_peak, __ATOMIC_ACQUIRE);
}

void kernel_overlap_sample(void) {
    if (!__atomic_load_n(&overlap_tracking, __ATOMIC_RELAXED)) return;
    uint32_t inside = defer_cpus_in_kernel();
    uint32_t peak = __atomic_load_n(&overlap_peak, __ATOMIC_RELAXED);
    while (inside > peak &&
           !__atomic_compare_exchange_n(&overlap_peak, &peak, inside, 0,
                                        __ATOMIC_RELAXED, __ATOMIC_RELAXED)) { }
}

void kernel_enter_from_isr(void) {
    defer_kernel_enter();
}

void kernel_leave_from_isr(void) {
    defer_kernel_leave();
    if (defer_in_kernel()) return;
    process_io_recheck();
    defer_poll();
}

void kernel_exit(void) {
    process_finish_switch();
    process_io_recheck();
    defer_kernel_leave();
    defer_poll();
}

void kernel_exit_from_isr(void) {
    process_finish_switch();
    kernel_leave_from_isr();
}
