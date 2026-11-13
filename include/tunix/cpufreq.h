#ifndef TUNIX_CPUFREQ_H
#define TUNIX_CPUFREQ_H

#include <stdint.h>

struct cpufreq_reading {
    uint8_t valid;
    uint32_t boot_ratio;
    uint32_t ratio;
    uint64_t effective_khz;
};

struct cpufreq_state {
    int eist_enabled;
    int measured;
    int requested;
    uint32_t min_ratio;
    uint32_t max_ratio;
    uint32_t target_ratio;
    int turbo;
    uint64_t ratio_khz;
    int smi_counted;
    uint64_t smi_count;
};

int cpufreq_supported(void);
void cpufreq_tick(void);
int cpufreq_read(unsigned cpu, struct cpufreq_reading *out);
const struct cpufreq_state *cpufreq_state(void);
void cpufreq_set_limit_khz(uint64_t khz);

#endif
