#ifndef TUNIX_THERMAL_H
#define TUNIX_THERMAL_H

#include <stdint.h>

struct thermal_reading {
    uint8_t valid;
    uint8_t throttled;
    int32_t celsius;
    int32_t peak;
    uint64_t sampled_ns;
};

struct thermal_state {
    int tjmax;
    int automatic_control;
    int clock_modulation;
    uint64_t throttle_events;
    int shutting_down;
};

int thermal_supported(void);
void thermal_tick(void);
int thermal_read(unsigned cpu, struct thermal_reading *out);
const struct thermal_state *thermal_state(void);

#endif
