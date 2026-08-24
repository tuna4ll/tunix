#ifndef TUNIX_TIMER_H
#define TUNIX_TIMER_H

#include <stdint.h>

struct interrupt_frame;

#define TIMER_FREQUENCY_HZ 250U

void timer_init(void);
void timer_irq(struct interrupt_frame *frame);
uint64_t timer_ticks(void);

#endif
