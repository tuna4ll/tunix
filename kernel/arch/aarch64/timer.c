#include <stdint.h>

#include "arch.h"

#define CNTP_CTL_ENABLE  (1UL << 0)
#define CNTP_CTL_IMASK   (1UL << 1)

static uint64_t interval;
static volatile uint64_t ticks;

void timer_init(void) {
    uint64_t freq = sysreg_read("cntfrq_el0");
    interval = freq / 100;                        // 10 ms tick
    sysreg_write("cntp_tval_el0", interval);
    sysreg_write("cntp_ctl_el0", CNTP_CTL_ENABLE);
    isb();
}

void timer_tick(void) {
    ticks++;
    sysreg_write("cntp_tval_el0", interval);       // rearm for the next tick
    sysreg_write("cntp_ctl_el0", CNTP_CTL_ENABLE);
}

uint64_t timer_ticks(void) {
    return ticks;
}
