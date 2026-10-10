#ifndef TUNIX_PROCESS_H
#define TUNIX_PROCESS_H
#include <stdint.h>
struct process {
    uint64_t pid;
};
void process_prepare_wait(const void *channel, uint64_t deadline_ns);
void process_wait(void);
void process_finish_wait(void);
int process_wake_all(const void *channel);
struct process *process_create_kthread(const char *name, void (*body)(void *), void *argument);
struct process *process_current(void);
#endif
