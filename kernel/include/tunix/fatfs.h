#ifndef TUNIX_FATFS_H
#define TUNIX_FATFS_H

#include <stdint.h>

struct vfs_node;

int fatfs_mount(const char *source, const char *mount_name, struct vfs_node **root_out);
void fatfs_unmount(struct vfs_node *root);
int fatfs_owns(const struct vfs_node *node);

#endif
