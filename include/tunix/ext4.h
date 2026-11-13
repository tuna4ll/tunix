#ifndef TUNIX_EXT4_H
#define TUNIX_EXT4_H

#include <stdint.h>

struct vfs_node;

struct ext4_fs_stats {
    uint64_t block_size;
    uint64_t blocks;
    uint64_t free_blocks;
    uint64_t reserved_blocks;
    uint64_t inodes;
    uint64_t free_inodes;
    uint32_t name_max;
    int read_only;
    uint8_t uuid[16];
};

int ext4fs_stats(const struct vfs_node *node, struct ext4_fs_stats *out);
int ext4fs_find_label(const char *label);
int ext4fs_mount_root(int read_only);
int ext4fs_mount(const char *source, const char *mount_name, int read_only,
                 struct vfs_node **root_out);
void ext4fs_unmount(struct vfs_node *root);
int ext4fs_owns(const struct vfs_node *node);
int ext4fs_read_only(const struct vfs_node *node);
const char *ext4fs_type_name(const struct vfs_node *node);
void ext4fs_start(void);
void ext4fs_balance(void);
int ext4fs_shutdown(void);

#endif
