#ifndef TUNIX_ACL_H
#define TUNIX_ACL_H

#include <stddef.h>
#include <stdint.h>

struct vfs_node;
struct credentials;

#define ACL_XATTR_ACCESS "system.posix_acl_access"
#define ACL_XATTR_DEFAULT "system.posix_acl_default"

int64_t acl_xattr_get(struct vfs_node *node, const char *name, void *out, size_t size);
int64_t acl_xattr_set(struct vfs_node *node, const char *name, const void *value, size_t size,
                      int flags);
int64_t acl_xattr_remove(struct vfs_node *node, const char *name);
int64_t acl_xattr_list(struct vfs_node *node, char *out, size_t size);
int acl_permission(const struct vfs_node *node, const struct credentials *cred, uint32_t want,
                   int *decided);
void acl_release(struct vfs_node *node);

#endif
