#include <stdint.h>
#include <tunix/drm.h>
#include <tunix/interrupt.h>
#include <tunix/iowait.h>
#include <tunix/net/net.h>
#include <tunix/percpu.h>
#include <tunix/process.h>
#include <tunix/sound.h>
#include <tunix/time.h>

#include <tunix/timer.h>
#include <tunix/vt.h>

#define TIMER_PERIOD_NS (1000000000ULL / TIMER_FREQUENCY_HZ)
#define TIMER_IDLE_NS   1000000000ULL

static volatile uint64_t ticks;

void timer_init(void) {
    ticks = 0;
    arch_timer_start(TIMER_FREQUENCY_HZ);
}

void timer_irq(struct interrupt_frame *frame) {
    ticks++;
    vt_poll_from_tick();
    net_tick();
    if ((ticks % (TIMER_FREQUENCY_HZ / 30U)) == 0U && vt_console_in_front()) drm_console_present();
    sound_tick();
    io_poll_tick();
    process_wake_io();
    process_io_recheck();
    process_timer_interrupt(frame);
}

uint64_t timer_ticks(void) { return ticks; }

static void program(struct cpu *cpu, uint64_t now) {
    uint64_t when = cpu->timer_deadline_ns < cpu->timer_next_tick_ns ? cpu->timer_deadline_ns
                                                                     : cpu->timer_next_tick_ns;
    if (when == UINT64_MAX) when = now + TIMER_IDLE_NS;
    cpu->timer_programmed_ns = when;
    arch_local_timer_program(when > now ? when - now : 0);
}

void timer_local_start(int with_ticks) {
    struct cpu *cpu = cpu_current();
    cpu->timer_local = 0;
    cpu->timer_due = 0;
    cpu->timer_next_tick_ns = UINT64_MAX;
    cpu->timer_deadline_ns = UINT64_MAX;
    cpu->timer_programmed_ns = UINT64_MAX;
    if (arch_local_timer_start() != 0) return;
    uint64_t now = time_uptime_ns();
    if (with_ticks) cpu->timer_next_tick_ns = now + TIMER_PERIOD_NS;
    cpu->timer_local = 1;
    program(cpu, now);
}

void timer_note_deadline(uint64_t deadline_ns) {
    struct cpu *cpu = cpu_current();
    if (!cpu || !cpu->timer_local || !deadline_ns || deadline_ns == UINT64_MAX) return;
    if (deadline_ns >= cpu->timer_deadline_ns) return;
    cpu->timer_deadline_ns = deadline_ns;
    if (deadline_ns < cpu->timer_programmed_ns) program(cpu, time_uptime_ns());
}

unsigned timer_local_expired(void) {
    struct cpu *cpu = cpu_current();
    if (!cpu->timer_local) return TIMER_LOCAL_TICK;
    uint64_t now = time_uptime_ns();
    unsigned due = 0;
    if (now >= cpu->timer_next_tick_ns) {
        due |= TIMER_LOCAL_TICK;
        cpu->timer_next_tick_ns += TIMER_PERIOD_NS;
        if (cpu->timer_next_tick_ns <= now) cpu->timer_next_tick_ns = now + TIMER_PERIOD_NS;
    }
    if (now >= cpu->timer_deadline_ns) {
        due |= TIMER_LOCAL_DEADLINE;
        cpu->timer_deadline_ns = UINT64_MAX;
    }
    program(cpu, now);
    return due;
}

void timer_run_deadlines(void) {
    process_expire_deadlines();
    process_wake_io();
    process_io_recheck();
}
