#ifndef TUNIX_SMP_H
#define TUNIX_SMP_H

#include <stdint.h>

#define SMP_TIMER_VECTOR 0xF0U
#define SMP_INVALIDATE_VECTOR 0xF1U
#define SMP_RESCHEDULE_VECTOR 0xF2U

void smp_init(void);

unsigned smp_cpu_count(void);

void smp_flush_address_space(uint64_t cr3);

void smp_flush_kernel_mappings(void);

void smp_service_flush(void);

void smp_flush_interrupt(void);
void smp_send_reschedule(void);

#endif
