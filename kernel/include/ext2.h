#ifndef TUNIX_EXT2_H
#define TUNIX_EXT2_H

#include <stdint.h>

struct vfs_node;

struct ext2_fs_stats {
    uint64_t block_size;
    uint64_t blocks;
    uint64_t free_blocks;
    uint64_t reserved_blocks;
    uint64_t inodes;
    uint64_t free_inodes;
};

/* 0 on success, -1 when no ext2 root is mounted. */
int ext2fs_stats(struct ext2_fs_stats *out);

int ext2fs_probe(uint32_t region_lba);
int ext2fs_mount_root(uint32_t region_lba);
int ext2fs_seed_root(uint32_t region_lba);
int ext2fs_mounted(void);
int ext2fs_owns(struct vfs_node *node);
int ext2fs_fsync_node(struct vfs_node *node);
int ext2fs_sync(void);

#endif
