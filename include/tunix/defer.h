#ifndef TUNIX_DEFER_H
#define TUNIX_DEFER_H

#include <stdint.h>

struct defer_item {
    struct defer_item *next;
    uint64_t generation;
    void (*release)(struct defer_item *item);
};

struct defer_park {
    struct defer_park *prev;
    struct defer_park *next;
    uint64_t entered;
    uint32_t depth;
    uint8_t listed;
};

void defer_kernel_enter(void);
void defer_park(struct defer_park *park);
void defer_unpark(struct defer_park *park);
void defer_cpu_reset(uint32_t depth);
void defer_kernel_leave(void);
int defer_in_kernel(void);
unsigned defer_cpus_in_kernel(void);
void defer_release(struct defer_item *item, void (*release)(struct defer_item *item));
void *defer_alloc(uint64_t size);
void defer_free(void *pointer);
void defer_poll(void);

#endif
