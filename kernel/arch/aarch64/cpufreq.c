#include <stddef.h>
#include <stdint.h>

#include "../../include/cpufreq.h"

static struct cpufreq_state state;

int cpufreq_supported(void) {
    return 0;
}

void cpufreq_tick(void) {
}

int cpufreq_read(unsigned index, struct cpufreq_reading *out) {
    (void)index;
    (void)out;
    return -1;
}

const struct cpufreq_state *cpufreq_state(void) {
    return &state;
}
