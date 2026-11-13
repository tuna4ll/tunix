#ifndef TUNIX_MUTEX_H
#define TUNIX_MUTEX_H

#include <stdint.h>
#include <tunix/lock.h>

struct process;
struct mutex_waiter;

struct mutex {
    struct lock guard;
    struct process *volatile owner;
    uint32_t depth;
    uint32_t rank;
    const char *name;
    struct mutex_waiter *first;
    struct mutex_waiter *last;
};

#define MUTEX_INITIALIZER(label, order) \
    {LOCK_INITIALIZER(label, LOCK_RANK_MUTEX), NULL, 0, (order), (label), NULL, NULL}

void mutex_init(struct mutex *mutex, const char *name, unsigned rank);
void mutex_lock(struct mutex *mutex);
int mutex_trylock(struct mutex *mutex);
void mutex_unlock(struct mutex *mutex);
int mutex_held(const struct mutex *mutex);
unsigned mutex_release_all(struct mutex *mutex);
void mutex_reacquire(struct mutex *mutex, unsigned depth);
void mutex_check_released(const char *where);

#define MUTEX_GUARD_TYPE __attribute__((cleanup(mutex_guard_release))) struct mutex *

static inline void mutex_guard_release(struct mutex **held) {
    if (*held) mutex_unlock(*held);
}

#endif
