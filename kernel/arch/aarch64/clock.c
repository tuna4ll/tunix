#include <stdint.h>

#include <tunix/time.h>
#include <tunix/timer.h>
#include "aarch64.h"

#define PL031_DATA 0x000U
#define DEFAULT_EPOCH 1767225600ULL
#define TIMER_MAX_DELAY_NS 1000000000ULL

uint64_t arch_clock_frequency(void) {
    uint64_t frequency;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(frequency));
    if (!frequency) frequency = aarch64_platform.timer_frequency;
    return frequency;
}

int arch_clock_invariant(void) { return 1; }

int arch_rtc_read(struct tunix_rtc_time *out) {
    uint64_t seconds = DEFAULT_EPOCH;
    if (aarch64_platform.rtc_base) {
        uint32_t value = *(volatile uint32_t *)(aarch64_platform.rtc_base + PL031_DATA);
        if (value) seconds = value;
    } else if (aarch64_platform.firmware_epoch) {
        uint64_t counter;
        __asm__ volatile("mrs %0, cntvct_el0" : "=r"(counter));
        uint64_t frequency = arch_clock_frequency();
        seconds = aarch64_platform.firmware_epoch +
                  (frequency ? (counter - aarch64_platform.firmware_epoch_counter) / frequency : 0);
    }
    time_epoch_to_calendar(seconds, out);
    return 0;
}

void arch_timer_start(unsigned hz) {
    (void)hz;
    timer_local_start(1);
}

int arch_local_timer_start(void) {
    if (!arch_clock_frequency()) return -1;
    __asm__ volatile("msr cntv_tval_el0, %0" : : "r"(0x7FFFFFFFULL));
    __asm__ volatile("msr cntv_ctl_el0, %0; isb" : : "r"(1ULL) : "memory");
    return 0;
}

void arch_local_timer_program(uint64_t delay_ns) {
    if (delay_ns > TIMER_MAX_DELAY_NS) delay_ns = TIMER_MAX_DELAY_NS;
    uint64_t count = (delay_ns * arch_clock_frequency() + 999999999ULL) / 1000000000ULL;
    if (!count) count = 1;
    if (count > 0x7FFFFFFFULL) count = 0x7FFFFFFFULL;
    __asm__ volatile("msr cntv_tval_el0, %0; isb" : : "r"(count) : "memory");
}
