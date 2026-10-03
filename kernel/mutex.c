#include <stddef.h>
#include <stdint.h>

#include "include/cpu.h"
#include "include/mutex.h"
#include "include/percpu.h"
#include "include/process.h"
#include "include/smp.h"

extern void kprintf(const char *fmt, ...);

#define MUTEX_REPORTS 16U

static volatile uint32_t reports;

static int may_report(void) {
    return __atomic_fetch_add(&reports, 1, __ATOMIC_RELAXED) < MUTEX_REPORTS;
}

static struct process *holder_token(void) {
    struct process *self = process_current();
    if (self) return self;
    return (struct process *)((uintptr_t)cpu_current() | 1U);
}

static int may_sleep(void) {
    return process_current() && !cpu_current()->in_interrupt && lock_only_holds(NULL);
}

static void check_order(struct process *self, const struct mutex *mutex) {
    uint32_t count = self->held_mutex_count < PROCESS_HELD_MUTEXES
                         ? self->held_mutex_count : PROCESS_HELD_MUTEXES;
    for (uint32_t index = 0; index < count; index++) {
        const struct mutex *other = self->held_mutexes[index];
        if (other->rank <= mutex->rank) continue;
        if (may_report())
            kprintf("LOCK: pid %u takes %s (rank %u) while holding %s (rank %u)\n",
                    (unsigned)self->pid, mutex->name, mutex->rank, other->name, other->rank);
        return;
    }
}

static void remember(struct process *self, struct mutex *mutex) {
    if (self->held_mutex_count < PROCESS_HELD_MUTEXES)
        self->held_mutexes[self->held_mutex_count] = mutex;
    self->held_mutex_count++;
}

static void forget(struct process *self, struct mutex *mutex) {
    if (!self->held_mutex_count) return;
    uint32_t top = self->held_mutex_count < PROCESS_HELD_MUTEXES
                       ? self->held_mutex_count : PROCESS_HELD_MUTEXES;
    for (uint32_t index = top; index-- > 0;) {
        if (self->held_mutexes[index] != mutex) continue;
        for (uint32_t move = index; move + 1U < top; move++)
            self->held_mutexes[move] = self->held_mutexes[move + 1U];
        break;
    }
    self->held_mutex_count--;
}

void mutex_init(struct mutex *mutex, const char *name, unsigned rank) {
    lock_init(&mutex->guard, name, LOCK_RANK_MUTEX);
    mutex->owner = NULL;
    mutex->depth = 0;
    mutex->waiters = 0;
    mutex->rank = rank;
    mutex->name = name;
}

int mutex_held(const struct mutex *mutex) {
    return mutex->owner == holder_token();
}

int mutex_trylock(struct mutex *mutex) {
    struct process *token = holder_token();
    if (mutex->owner == token) {
        mutex->depth++;
        return 1;
    }
    lock_acquire(&mutex->guard);
    int taken = mutex->owner == NULL;
    if (taken) {
        mutex->owner = token;
        mutex->depth = 1;
    }
    lock_release(&mutex->guard);
    if (taken && process_current()) remember(process_current(), mutex);
    return taken;
}

void mutex_lock(struct mutex *mutex) {
    struct process *self = process_current();
    struct process *token = holder_token();
    if (mutex->owner == token) {
        mutex->depth++;
        return;
    }
    int sleeping = may_sleep();
    if (self) {
        if (!sleeping) lock_report_sleep(mutex->name);
        check_order(self, mutex);
    }
    for (;;) {
        lock_acquire(&mutex->guard);
        if (!mutex->owner) {
            mutex->owner = token;
            mutex->depth = 1;
            lock_release(&mutex->guard);
            if (self) remember(self, mutex);
            return;
        }
        if (!sleeping) {
            lock_release(&mutex->guard);
            smp_service_flush();
            cpu_relax();
            continue;
        }
        mutex->waiters++;
        process_prepare_wait(mutex, 0);
        lock_release(&mutex->guard);
        process_wait();
        process_finish_wait();
        lock_acquire(&mutex->guard);
        mutex->waiters--;
        lock_release(&mutex->guard);
    }
}

void mutex_unlock(struct mutex *mutex) {
    struct process *token = holder_token();
    if (mutex->owner != token) {
        if (may_report())
            kprintf("LOCK: releases %s it does not hold\n", mutex->name);
        return;
    }
    if (--mutex->depth) return;
    struct process *self = process_current();
    if (self) forget(self, mutex);
    lock_acquire(&mutex->guard);
    mutex->owner = NULL;
    int waiting = mutex->waiters != 0;
    lock_release(&mutex->guard);
    if (waiting) process_wake_one(mutex);
}

unsigned mutex_release_all(struct mutex *mutex) {
    if (!mutex_held(mutex)) return 0;
    unsigned depth = mutex->depth;
    mutex->depth = 1;
    mutex_unlock(mutex);
    return depth;
}

void mutex_reacquire(struct mutex *mutex, unsigned depth) {
    if (!depth) return;
    mutex_lock(mutex);
    mutex->depth = depth;
}

void mutex_check_released(const char *where) {
    struct process *self = process_current();
    if (!self || !self->held_mutex_count) return;
    if (may_report())
        kprintf("LOCK: pid %u leaves %s holding %u mutex(es), first %s\n",
                (unsigned)self->pid, where, (unsigned)self->held_mutex_count,
                self->held_mutexes[0]->name);
}
