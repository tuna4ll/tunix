#ifndef TUNIX_WORKQUEUE_H
#define TUNIX_WORKQUEUE_H

#include <stdint.h>

struct work {
    void (*run)(void *argument);
    void *argument;
    struct work *next;
    volatile uint32_t queued;
};

#define WORK_INITIALIZER(function, data) {(function), (data), 0, 0}

void workqueue_init(void);
void work_queue(struct work *work);

#endif
