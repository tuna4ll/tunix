#ifndef SPINLOCK_H
#define SPINLOCK_H

#include <stdint.h>

typedef struct {
    volatile uint32_t lock;
} spinlock_t;

static inline void spinlock_init(spinlock_t* sl) {
    sl->lock = 0;
}

static inline void spinlock_acquire(spinlock_t* sl) {
    while (__sync_lock_test_and_set(&sl->lock, 1)) {
        __asm__ volatile("pause");
    }
}

static inline void spinlock_release(spinlock_t* sl) {
    __sync_lock_release(&sl->lock);
}

#endif
