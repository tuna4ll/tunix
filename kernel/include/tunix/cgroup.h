#ifndef TUNIX_CGROUP_H
#define TUNIX_CGROUP_H

#include <stddef.h>
#include <stdint.h>

#define CGROUP_HIERARCHIES 4
#define CGROUP2_SUPER_MAGIC 0x63677270U
#define CGROUP_SUPER_MAGIC 0x0027E0EBU

struct cgroup;
struct process;
struct vfs_node;

int cgroupfs_mount(const char *type, const char *options, const char *name,
                   struct vfs_node **root);
int cgroupfs_unmount(struct vfs_node *root);
uint32_t cgroupfs_magic(const struct vfs_node *node);
int cgroupfs_is_cgroup(const struct vfs_node *node);
int cgroupfs_rmdir(struct vfs_node *directory);

void cgroup_fork(struct process *parent, struct process *child);
void cgroup_exit(struct process *process);
void cgroup_drop(struct process *process);
size_t cgroup_describe(struct process *process, char *out, size_t capacity);

#endif
