#ifndef TUNIX_EXT2_H
#define TUNIX_EXT2_H

#include <stdint.h>

struct vfs_node;

int ext2fs_probe(uint32_t region_lba);
int ext2fs_mount_root(uint32_t region_lba);
int ext2fs_seed_root(uint32_t region_lba);
int ext2fs_mounted(void);
int ext2fs_owns(struct vfs_node *node);
int ext2fs_fsync_node(struct vfs_node *node);
int ext2fs_sync(void);

#endif
