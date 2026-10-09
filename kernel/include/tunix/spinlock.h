#ifndef SPINLOCK_H
#define SPINLOCK_H

#include <tunix/lock.h>

typedef struct lock spinlock_t;

static inline void spinlock_init(spinlock_t *sl) {
    lock_init(sl, "object", LOCK_RANK_OBJECT);
}

static inline void spinlock_acquire(spinlock_t *sl) {
    lock_acquire(sl);
}

static inline void spinlock_release(spinlock_t *sl) {
    lock_release(sl);
}

#endif
