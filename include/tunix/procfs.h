#ifndef TUNIX_PROCFS_H
#define TUNIX_PROCFS_H

#include <stdint.h>

struct process;

void procfs_init(void);
void procfs_register_process(struct process *process);
void procfs_unregister_process(uint64_t pid);
struct vfs_node;
// For a /proc/PID/ns entry: the pid and the kind ("net", "pid_for_children"...).
int procfs_ns_entry(struct vfs_node *node, uint64_t *pid, char *kind, unsigned capacity);

#endif
