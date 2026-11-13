#ifndef TUNIX_PROCFS_H
#define TUNIX_PROCFS_H

#include <stdint.h>

struct process;

void procfs_init(void);
void procfs_register_process(struct process *process);
void procfs_unregister_process(uint64_t pid);

#endif
