#ifndef TUNIX_LOCK_H
#define TUNIX_LOCK_H

#include <stdint.h>

enum lock_rank {
    LOCK_RANK_TTY = 6,
    LOCK_RANK_CHAR = 8,
    LOCK_RANK_FILE = 9,
    LOCK_RANK_MEMORY = 10,
    LOCK_RANK_MODULES = 18,
    LOCK_RANK_VFS = 20,
    LOCK_RANK_EXT2 = 25,
    LOCK_RANK_OBJECT = 30,
    LOCK_RANK_NET = 35,
    LOCK_RANK_DEVICE = 40,
    LOCK_RANK_BLOCK = 44,
    LOCK_RANK_BUS = 45,
    LOCK_RANK_INPUT = 48,
    LOCK_RANK_FILES = 50,
    LOCK_RANK_PAGE_TABLES = 60,
    LOCK_RANK_MUTEX = 69,
    LOCK_RANK_SCHED = 70,
    LOCK_RANK_EVENTS = 72,
    LOCK_RANK_REGISTRY = 74,
    LOCK_RANK_HEAP = 75,
    LOCK_RANK_KERNEL_MAP = 80,
    LOCK_RANK_PAGES = 85,
    LOCK_RANK_LEAF = 90,
};

struct lock {
    volatile uint32_t next;
    volatile uint32_t serving;
    volatile uint32_t owner;
    uint32_t depth;
    uint32_t rank;
    const char *name;
};

#define LOCK_INITIALIZER(label, order) { 0, 0, 0, 0, (order), (label) }

void lock_init(struct lock *lock, const char *name, unsigned rank);
void lock_acquire(struct lock *lock);
int lock_try_acquire(struct lock *lock);
void lock_release(struct lock *lock);
void lock_drop(struct lock *lock);
int lock_held(const struct lock *lock);
unsigned lock_depth_here(void);
void lock_check_released(const char *where);
unsigned lock_depth(const struct lock *lock);
void lock_set_depth(struct lock *lock, unsigned depth);
int lock_only_holds(const struct lock *lock);
void lock_report_sleep(const char *what);

#endif
