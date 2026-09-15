#include <stdint.h>
#include "include/drm.h"
#include "include/interrupt.h"
#include "include/process.h"
#include "include/sound.h"

#include "include/timer.h"
#include "include/vt.h"

static volatile uint64_t ticks;

void timer_init(void) {
    ticks = 0;
    arch_timer_start(TIMER_FREQUENCY_HZ);
}

void timer_irq(struct interrupt_frame *frame) {
    ticks++;
    vt_poll_input();
    if ((ticks % (TIMER_FREQUENCY_HZ / 30U)) == 0U && vt_console_in_front())
        drm_console_present();
    sound_tick();
    process_wake_io();
    process_timer_interrupt(frame);
}

uint64_t timer_ticks(void) {
    return ticks;
}
