#include <stddef.h>
#include <stdint.h>

#include <tunix/thermal.h>

static struct thermal_state state;

int thermal_supported(void) {
    return 0;
}

void thermal_tick(void) {
}

int thermal_read(unsigned index, struct thermal_reading *out) {
    (void)index;
    (void)out;
    return -1;
}

const struct thermal_state *thermal_state(void) {
    return &state;
}
