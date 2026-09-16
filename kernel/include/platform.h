#ifndef TUNIX_PLATFORM_H
#define TUNIX_PLATFORM_H

void arch_early_init(void);
void arch_cpu_init(void);
void arch_route_legacy_interrupts(void);
void arch_route_timer(void);

#if defined(__x86_64__)
#include "acpi.h"

static inline void arch_probe_buses(void) {
}

static inline void arch_power_off(void) {
    acpi_power_off();
}

__attribute__((noreturn)) static inline void arch_restart(void) {
    acpi_reset();
}
#else
void arch_probe_buses(void);
void arch_power_off(void);
void arch_restart(void) __attribute__((noreturn));
#endif

#endif
