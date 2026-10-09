#ifndef TUNIX_APIC_H
#define TUNIX_APIC_H

#include <stdint.h>

int apic_init(void);

int apic_is_active(void);
void apic_send_eoi(void);

int apic_route_legacy_irq(unsigned irq);

int apic_route_irq(unsigned global, unsigned vector, int active_low, int level_triggered);

int apic_route_global(unsigned global, unsigned vector);

uint32_t apic_local_id(void);
void apic_enable_local(void);

void apic_send_init(uint32_t apic_id);
void apic_send_startup(uint32_t apic_id, uint8_t page);
void apic_send_ipi(uint32_t apic_id, uint8_t vector);
void apic_send_ipi_to_others(uint8_t vector);

void apic_timer_calibration(unsigned index, uint64_t *measured_hz, uint32_t *initial_count);

#endif
