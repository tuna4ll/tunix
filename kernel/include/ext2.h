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

int ext2fs_stats(const struct vfs_node *node, struct ext2_fs_stats *out);

int ext2fs_find_label(const char *label);

int ext2fs_mount_root(void);
int ext2fs_mount(const char *source, const char *mount_name, struct vfs_node **root_out);
void ext2fs_unmount(struct vfs_node *root);
int ext2fs_owns(const struct vfs_node *node);
void ext2fs_start(void);
int ext2fs_journalled(const struct vfs_node *node);
int ext2fs_shutdown(void);

#endif
