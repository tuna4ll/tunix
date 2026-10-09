#ifndef TUNIX_TIMER_H
#define TUNIX_TIMER_H

#include <stdint.h>

struct interrupt_frame;

#define TIMER_FREQUENCY_HZ 250U
#define TIMER_LOCAL_TICK 1U
#define TIMER_LOCAL_DEADLINE 2U

void timer_init(void);
void arch_timer_start(unsigned hz);
void timer_irq(struct interrupt_frame *frame);
uint64_t timer_ticks(void);
void timer_local_start(int ticks);
unsigned timer_local_expired(void);
void timer_note_deadline(uint64_t deadline_ns);
void timer_run_deadlines(void);
int arch_local_timer_start(void);
void arch_local_timer_program(uint64_t delay_ns);

#endif
