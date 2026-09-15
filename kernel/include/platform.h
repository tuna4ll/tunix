#ifndef TUNIX_PLATFORM_H
#define TUNIX_PLATFORM_H

void arch_early_init(void);
void arch_cpu_init(void);
void arch_route_legacy_interrupts(void);
void arch_route_timer(void);

#endif
