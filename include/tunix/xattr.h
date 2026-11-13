#ifndef TUNIX_XATTR_H
#define TUNIX_XATTR_H

#include <stddef.h>
#include <stdint.h>

struct vfs_node;

struct vfs_xattr {
    struct vfs_xattr *next;
    uint32_t size;
    uint8_t *value;
    char name[];
};

#define VFS_XATTR_NAME_MAX  255U
#define VFS_XATTR_TOTAL_MAX 65536U

int64_t vfs_xattr_get(struct vfs_node *node, const char *name, void *out, size_t size);
int64_t vfs_xattr_set(struct vfs_node *node, const char *name, const void *value, size_t size,
                      int flags);
int64_t vfs_xattr_remove(struct vfs_node *node, const char *name);
int64_t vfs_xattr_list(struct vfs_node *node, char *out, size_t size);
int vfs_xattr_import(struct vfs_node *node, const char *name, const void *value, size_t size);
void vfs_xattr_release(struct vfs_node *node);
const struct vfs_xattr *vfs_xattr_first(const struct vfs_node *node);

#endif
