#include <stdint.h>

#include "../../include/time.h"
#include "../../include/timer.h"
#include "aarch64.h"

#define PL031_DATA 0x000U
#define DEFAULT_EPOCH 1767225600ULL

static uint64_t period;

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
    period = arch_clock_frequency() / hz;
    __asm__ volatile("msr cntv_tval_el0, %0" : : "r"(period));
    __asm__ volatile("msr cntv_ctl_el0, %0; isb" : : "r"(1ULL) : "memory");
}

void aarch64_timer_rearm(void) {
    __asm__ volatile("msr cntv_tval_el0, %0" : : "r"(period));
}
