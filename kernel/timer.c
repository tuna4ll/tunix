#include <stdint.h>
#include "include/drm.h"
#include "include/interrupt.h"
#include "include/io.h"
#include "include/process.h"
#include "include/timer.h"
#include "include/vt.h"

#define PIT_INPUT_HZ 1193182U
#define PIT_COMMAND 0x43U
#define PIT_CHANNEL0 0x40U
#define PIT_MODE_RATE_GENERATOR 0x34U

static volatile uint64_t ticks;

void timer_init(void) {
    uint32_t divisor = (PIT_INPUT_HZ + TIMER_FREQUENCY_HZ / 2U) /
                       TIMER_FREQUENCY_HZ;
    if (divisor < 1U) divisor = 1U;
    if (divisor > 0xFFFFU) divisor = 0xFFFFU;

    ticks = 0;
    outb(PIT_COMMAND, PIT_MODE_RATE_GENERATOR);
    outb(PIT_CHANNEL0, (uint8_t)(divisor & 0xFFU));
    outb(PIT_CHANNEL0, (uint8_t)(divisor >> 8));
}

void timer_irq(struct interrupt_frame *frame) {
    ticks++;
    /*
     * The two keyboards that raise no interrupt: the serial line, which nothing
     * unmasks IRQ 4 for, and USB, whose event ring is read rather than
     * delivered. Both used to be looked at by whoever was waiting to read a
     * terminal, and terminal reads now sleep instead of spinning -- so the tick
     * is what looks. It costs one port read and one memory read when nothing
     * has happened, which is almost always.
     */
    vt_poll_input();
    /* Roughly thirty times a second, and only while the console owns the
       screen: see drm_console_present(). */
    if ((ticks % (TIMER_FREQUENCY_HZ / 30U)) == 0U && vt_console_in_front())
        drm_console_present();
    /*
     * Release whatever is waiting on the general channel. Most readiness has
     * a wakeup of its own, but some has none at all -- a packet arriving for
     * a socket is noticed by polling the adapter, not by anything that could
     * signal a sleeper -- and without this a poll() on one of those would
     * wait for its timeout instead of its data. Waking a sleeper that has
     * nothing to do costs it one re-test.
     */
    process_wake_io();
    process_timer_interrupt(frame);
}

uint64_t timer_ticks(void) {
    return ticks;
}
