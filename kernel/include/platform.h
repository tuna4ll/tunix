#ifndef TUNIX_PLATFORM_H
#define TUNIX_PLATFORM_H

void arch_early_init(void);
void arch_cpu_init(void);
void arch_route_legacy_interrupts(void);
void arch_route_timer(void);

#if defined(__x86_64__)
static inline void arch_probe_buses(void) {
}
#else
void arch_probe_buses(void);
#endif

#endif
