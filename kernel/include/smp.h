#ifndef TUNIX_SMP_H
#define TUNIX_SMP_H

#include <stdint.h>

/* The vectors the processors use to reach each other and themselves. High, so
   they cannot collide with a device line the IOAPIC might route. */
#define SMP_TIMER_VECTOR 0xF0U
#define SMP_INVALIDATE_VECTOR 0xF1U

/* Start every processor the firmware listed. Safe to call on a machine with
   one: it says so and returns. Must run after the APIC, the timer and the
   syscall entry are set up, and before the first process starts. */
void smp_init(void);
/* How many are running, the first one included. */
unsigned smp_cpu_count(void);

/*
 * Tell every other processor that has this address space loaded to throw away
 * its cached translations, and wait until they all have.
 *
 * The waiting is what makes it safe to free the page afterwards: a processor
 * still holding a translation to it would keep writing into memory that has
 * been handed to somebody else. It cannot deadlock, because a processor that
 * cannot answer the interrupt -- one waiting for the kernel lock, with
 * interrupts off -- answers the request in its wait loop instead.
 */
void smp_flush_address_space(uint64_t cr3);
/* Answer a request if one is outstanding. Called from the kernel lock's wait
   loop and from the interrupt that asks -- the two places a processor can be
   reached. */
void smp_service_flush(void);
/* The interrupt handler's own half of it, called from isr.S. */
void smp_flush_interrupt(void);

#endif
