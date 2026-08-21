#ifndef TUNIX_FATFS_H
#define TUNIX_FATFS_H

#include <stdint.h>

struct vfs_node;

/*
 * FAT, the filesystem a USB stick arrives formatted with.
 *
 * Mount builds the directory tree in memory -- directories are small and
 * walking them once is cheaper than walking them per lookup -- while file
 * contents stay on the medium and are read through the node's own read handler
 * when someone asks. That is why this needs no share of the persistence hooks
 * the ext2 driver owns: nothing here goes through the VFS data cache.
 *
 * `source` names a block device as /dev/sdX. Returns 0 and stores the tree's
 * root, or a negative errno.
 */
int fatfs_mount(const char *source, const char *mount_name, struct vfs_node **root_out);
/* Release everything a mount allocated, including the tree. */
void fatfs_unmount(struct vfs_node *root);
/* Whether this node belongs to a FAT mount. */
int fatfs_owns(const struct vfs_node *node);

#endif
