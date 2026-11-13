#include <stddef.h>
#include <stdint.h>

#include <tunix/heap.h>
#include <tunix/kstring.h>
#include <tunix/posix_timer.h>
#include <tunix/process.h>
#include <tunix/signal.h>
#include <tunix/smp.h>
#include <tunix/time.h>
#include <tunix/timer.h>
#include <tunix/errno.h>

#define CPUCLOCK_PERTHREAD 4U
#define CPUCLOCK_KIND_MASK 3U
#define CPUCLOCK_FD        3U
#define CLOCK_COARSE_NS    (1000000000ULL / TIMER_FREQUENCY_HZ)
#define TIMER_ID_MAX       INT32_MAX

struct posix_timer {
    struct posix_timer *next;
    uint64_t tgid;
    int32_t id;
    int clock;
    int notify;
    uint64_t target_tid;
    uint64_t cpu_pid;
    int cpu_thread;
    uint64_t interval_ns;
    uint64_t deadline_ns;
    uint64_t overrun_pending;
    int32_t overrun_last;
    struct process *queued_on;
    struct signal_entry entry;
};

static struct posix_timer *timers;

static uint64_t saturating_add(uint64_t a, uint64_t b) {
    return a > UINT64_MAX - b ? UINT64_MAX : a + b;
}

static int cpu_clock_target(int clock, uint64_t *pid, int *thread) {
    struct process *self = process_current();
    if (!self) return -EINVAL;
    if (clock == CLOCK_PROCESS_CPUTIME_ID || clock == CLOCK_THREAD_CPUTIME_ID) {
        *thread = clock == CLOCK_THREAD_CPUTIME_ID;
        *pid = *thread ? self->pid : self->tgid;
        return 0;
    }
    if (clock >= 0 || ((unsigned)clock & CPUCLOCK_KIND_MASK) == CPUCLOCK_FD) return -EINVAL;
    *thread = ((unsigned)clock & CPUCLOCK_PERTHREAD) != 0;
    int64_t encoded = ~((int64_t)clock >> 3);
    if (encoded < 0) return -EINVAL;
    *pid = encoded ? (uint64_t)encoded : (*thread ? self->pid : self->tgid);
    struct process *target = process_find(*pid);
    if (!target || target->state == PROCESS_DEAD) return -EINVAL;
    if (*thread && target->tgid != self->tgid) return -EINVAL;
    if (!*thread && target->tgid != target->pid) return -EINVAL;
    return 0;
}

static int cpu_clock(int clock) { return clock < 0 || clock == 2 || clock == 3; }

static uint64_t cpu_time_of(uint64_t pid, int thread) {
    struct process *target = process_find(pid);
    return target ? process_cpu_time_locked(target, thread) : 0;
}

static uint64_t wall_now(int clock) {
    if (clock == CLOCK_REALTIME || clock == CLOCK_REALTIME_COARSE ||
        clock == CLOCK_REALTIME_ALARM || clock == CLOCK_TAI)
        return time_realtime_ns();
    return time_uptime_ns();
}

int posix_clock_read(int clock, uint64_t *ns) {
    if (cpu_clock(clock)) {
        process_table_lock();
        uint64_t pid = 0;
        int thread = 0;
        int status = cpu_clock_target(clock, &pid, &thread);
        if (status == 0) *ns = cpu_time_of(pid, thread);
        process_table_unlock();
        return status;
    }
    switch (clock) {
    case CLOCK_REALTIME:
    case CLOCK_MONOTONIC:
    case CLOCK_MONOTONIC_RAW:
    case CLOCK_REALTIME_COARSE:
    case CLOCK_MONOTONIC_COARSE:
    case CLOCK_BOOTTIME:
    case CLOCK_REALTIME_ALARM:
    case CLOCK_BOOTTIME_ALARM:
    case CLOCK_TAI:              *ns = wall_now(clock); return 0;
    default:                     return -EINVAL;
    }
}

int posix_clock_resolution(int clock, uint64_t *ns) {
    uint64_t ignored;
    int status = posix_clock_read(clock, &ignored);
    if (status != 0) return status;
    *ns = clock == CLOCK_REALTIME_COARSE || clock == CLOCK_MONOTONIC_COARSE ? CLOCK_COARSE_NS : 1;
    return 0;
}

static struct posix_timer *find_timer(uint64_t tgid, int32_t id) {
    for (struct posix_timer *timer = timers; timer; timer = timer->next)
        if (timer->tgid == tgid && timer->id == id) return timer;
    return NULL;
}

static int32_t free_timer_id(uint64_t tgid) {
    for (int32_t id = 0; id < TIMER_ID_MAX; id++)
        if (!find_timer(tgid, id)) return id;
    return -1;
}

static uint64_t clock_now(const struct posix_timer *timer) {
    return cpu_clock(timer->clock) ? cpu_time_of(timer->cpu_pid, timer->cpu_thread)
                                   : wall_now(timer->clock);
}

static uint64_t uptime_deadline(const struct posix_timer *timer, uint64_t now) {
    if (!timer->deadline_ns) return 0;
    uint64_t current = clock_now(timer);
    uint64_t remaining = timer->deadline_ns > current ? timer->deadline_ns - current : 0;
    if (cpu_clock(timer->clock)) {
        unsigned cpus = smp_cpu_count();
        remaining /= cpus ? cpus : 1U;
        if (remaining < CLOCK_COARSE_NS) remaining = CLOCK_COARSE_NS;
    }
    return saturating_add(now, remaining);
}

int posix_timer_create(int clock, const struct posix_timer_event *event, int32_t *id) {
    struct process *self = process_current();
    if (!self || !id) return -EINVAL;
    struct posix_timer_event chosen = {0, SIGALRM, SIGEV_SIGNAL, 0};
    if (event) chosen = *event;
    int kind = chosen.notify & ~SIGEV_THREAD_ID;
    if (kind != SIGEV_SIGNAL && kind != SIGEV_NONE) return -EINVAL;
    if ((chosen.notify & SIGEV_THREAD_ID) && kind != SIGEV_SIGNAL) return -EINVAL;
    if (kind == SIGEV_SIGNAL && (chosen.signo < 1 || chosen.signo > TUNIX_NSIG)) return -EINVAL;
    if (clock == CLOCK_MONOTONIC_RAW || clock == CLOCK_REALTIME_COARSE ||
        clock == CLOCK_MONOTONIC_COARSE)
        return -EOPNOTSUPP;
    uint64_t ignored;
    int status = posix_clock_read(clock, &ignored);
    if (status != 0) return status;

    struct posix_timer *timer = (struct posix_timer *)kmalloc(sizeof(*timer));
    if (!timer) return -ENOMEM;
    memset(timer, 0, sizeof(*timer));
    timer->tgid = self->tgid;
    timer->clock = clock;
    timer->notify = chosen.notify;

    process_table_lock();
    if (cpu_clock(clock)) status = cpu_clock_target(clock, &timer->cpu_pid, &timer->cpu_thread);
    if (status == 0 && (chosen.notify & SIGEV_THREAD_ID)) {
        struct process *target = process_find((uint64_t)(uint32_t)chosen.thread);
        if (chosen.thread <= 0 || !target || target->tgid != self->tgid) status = -EINVAL;
        else timer->target_tid = target->pid;
    }
    int32_t chosen_id = status == 0 ? free_timer_id(self->tgid) : -1;
    if (status == 0 && chosen_id < 0) status = -EAGAIN;
    if (status != 0) {
        process_table_unlock();
        kfree(timer);
        return status;
    }
    timer->id = chosen_id;
    timer->entry.timer = timer;
    timer->entry.info.signo = chosen.signo;
    timer->entry.info.code = SI_TIMER;
    timer->entry.info.fields.timer.id = chosen_id;
    timer->entry.info.fields.timer.value = event ? chosen.value : (uint64_t)(uint32_t)chosen_id;
    timer->next = timers;
    timers = timer;
    process_table_unlock();
    *id = chosen_id;
    return 0;
}

static void current_spec(const struct posix_timer *timer, struct posix_timer_spec *spec) {
    spec->interval_ns = timer->interval_ns;
    spec->value_ns = 0;
    if (!timer->deadline_ns) return;
    uint64_t now = clock_now(timer);
    spec->value_ns = timer->deadline_ns > now ? timer->deadline_ns - now : 1;
}

int posix_timer_settime(int32_t id, int flags, const struct posix_timer_spec *value,
                        struct posix_timer_spec *old) {
    struct process *self = process_current();
    if (!self || !value) return -EINVAL;
    if (flags & ~TIMER_ABSTIME) return -EINVAL;
    process_table_lock();
    struct posix_timer *timer = find_timer(self->tgid, id);
    if (!timer) {
        process_table_unlock();
        return -EINVAL;
    }
    if (old) current_spec(timer, old);
    timer->interval_ns = value->interval_ns;
    timer->overrun_pending = 0;
    timer->overrun_last = 0;
    if (!value->value_ns) {
        timer->deadline_ns = 0;
    } else {
        uint64_t now = clock_now(timer);
        uint64_t deadline =
            (flags & TIMER_ABSTIME) ? value->value_ns : saturating_add(now, value->value_ns);
        timer->deadline_ns = deadline ? deadline : 1;
    }
    uint64_t wake = uptime_deadline(timer, time_uptime_ns());
    process_table_unlock();
    if (wake) process_note_deadline(wake);
    return 0;
}

int posix_timer_gettime(int32_t id, struct posix_timer_spec *value) {
    struct process *self = process_current();
    if (!self || !value) return -EINVAL;
    process_table_lock();
    struct posix_timer *timer = find_timer(self->tgid, id);
    if (timer) current_spec(timer, value);
    process_table_unlock();
    return timer ? 0 : -EINVAL;
}

int posix_timer_getoverrun(int32_t id) {
    struct process *self = process_current();
    if (!self) return -EINVAL;
    process_table_lock();
    struct posix_timer *timer = find_timer(self->tgid, id);
    int result = timer ? timer->overrun_last : -EINVAL;
    process_table_unlock();
    return result;
}

static void unlink_timer(struct posix_timer *timer) {
    struct posix_timer **link = &timers;
    while (*link && *link != timer) link = &(*link)->next;
    if (*link) *link = timer->next;
    if (timer->queued_on) process_unqueue_signal_entry_locked(timer->queued_on, &timer->entry);
    timer->queued_on = NULL;
}

int posix_timer_delete(int32_t id) {
    struct process *self = process_current();
    if (!self) return -EINVAL;
    process_table_lock();
    struct posix_timer *timer = find_timer(self->tgid, id);
    if (timer) unlink_timer(timer);
    process_table_unlock();
    if (!timer) return -EINVAL;
    kfree(timer);
    return 0;
}

void posix_timers_release(uint64_t tgid) {
    for (;;) {
        process_table_lock();
        struct posix_timer *timer = timers;
        while (timer && timer->tgid != tgid) timer = timer->next;
        if (timer) unlink_timer(timer);
        process_table_unlock();
        if (!timer) return;
        kfree(timer);
    }
}

void posix_timer_dequeued(struct posix_timer *timer) { timer->queued_on = NULL; }

int32_t posix_timer_overrun(struct posix_timer *timer) {
    timer->overrun_last =
        timer->overrun_pending > INT32_MAX ? INT32_MAX : (int32_t)timer->overrun_pending;
    timer->overrun_pending = 0;
    return timer->overrun_last;
}

static void fire(struct posix_timer *timer) {
    if ((timer->notify & ~SIGEV_THREAD_ID) == SIGEV_NONE) return;
    if (timer->queued_on) {
        timer->overrun_pending++;
        return;
    }
    struct process *target = process_find(timer->target_tid ? timer->target_tid : timer->tgid);
    if (!target || target->tgid != timer->tgid) return;
    if (process_queue_signal_entry_locked(target, &timer->entry) == 0) timer->queued_on = target;
    else timer->overrun_pending++;
}

uint64_t posix_timers_expire(uint64_t now) {
    uint64_t earliest = 0;
    for (struct posix_timer *timer = timers; timer; timer = timer->next) {
        if (!timer->deadline_ns) continue;
        uint64_t current = clock_now(timer);
        if (current >= timer->deadline_ns) {
            if (timer->interval_ns) {
                uint64_t periods = 1 + (current - timer->deadline_ns) / timer->interval_ns;
                timer->overrun_pending += periods - 1;
                uint64_t advance = periods > UINT64_MAX / timer->interval_ns
                    ? UINT64_MAX
                    : periods * timer->interval_ns;
                timer->deadline_ns = saturating_add(timer->deadline_ns, advance);
            } else {
                timer->deadline_ns = 0;
            }
            fire(timer);
        }
        uint64_t wake = uptime_deadline(timer, now);
        if (wake && (!earliest || wake < earliest)) earliest = wake;
    }
    return earliest;
}
