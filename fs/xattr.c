#include <stddef.h>
#include <stdint.h>
#include <tunix/cred.h>
#include <tunix/heap.h>
#include <tunix/kstring.h>
#include <tunix/vfs.h>
#include <tunix/xattr.h>
#include <tunix/errno.h>

#define XATTR_CREATE  1
#define XATTR_REPLACE 2

static const char *const namespaces[] = {"user.", "trusted.", "security."};

static int known_namespace(const char *name) {
    for (size_t index = 0; index < sizeof(namespaces) / sizeof(namespaces[0]); index++) {
        size_t length = strlen(namespaces[index]);
        if (strncmp(name, namespaces[index], length) == 0 && name[length]) return (int)index;
    }
    return -1;
}

static int privileged(void) {
    const struct credentials *cred = cred_current();
    return !cred || cred->fsuid == 0;
}

static int may_read(const struct vfs_node *node, int space) {
    (void)node;
    if (space == 1 && !privileged()) return -ENODATA;
    return 0;
}

static int may_write(struct vfs_node *node, int space) {
    if (node->flags & VFS_READONLY) return -EROFS;
    if (space == 0) {
        uint32_t kind = node->flags & 0xFFU;
        if (kind != VFS_FILE && kind != VFS_DIRECTORY) return -EPERM;
        if (cred_may(node, CRED_WRITE) != 0) return -EACCES;
        return 0;
    }
    return privileged() ? 0 : -EPERM;
}

static struct vfs_xattr *find(const struct vfs_node *node, const char *name) {
    for (struct vfs_xattr *attr = node->xattrs; attr; attr = attr->next)
        if (strcmp(attr->name, name) == 0) return attr;
    return NULL;
}

static size_t total_bytes(const struct vfs_node *node) {
    size_t total = 0;
    for (struct vfs_xattr *attr = node->xattrs; attr; attr = attr->next)
        total += strlen(attr->name) + 1U + attr->size;
    return total;
}

static void unlink_attr(struct vfs_node *node, struct vfs_xattr *target) {
    for (struct vfs_xattr **link = &node->xattrs; *link; link = &(*link)->next) {
        if (*link != target) continue;
        *link = target->next;
        kfree(target->value);
        kfree(target);
        return;
    }
}

static int store(struct vfs_node *node, const char *name, const void *value, size_t size) {
    size_t length = strlen(name);
    struct vfs_xattr *attr = (struct vfs_xattr *)kmalloc(sizeof(*attr) + length + 1U);
    if (!attr) return -ENOMEM;
    attr->value = NULL;
    if (size) {
        attr->value = (uint8_t *)kmalloc(size);
        if (!attr->value) {
            kfree(attr);
            return -ENOMEM;
        }
        memcpy(attr->value, value, size);
    }
    attr->size = (uint32_t)size;
    memcpy(attr->name, name, length + 1U);
    struct vfs_xattr *old = find(node, name);
    if (old) unlink_attr(node, old);
    struct vfs_xattr **tail = &node->xattrs;
    while (*tail) tail = &(*tail)->next;
    attr->next = NULL;
    *tail = attr;
    return 0;
}

int64_t vfs_xattr_get(struct vfs_node *node, const char *name, void *out, size_t size) {
    int space = known_namespace(name);
    if (space < 0) return -EOPNOTSUPP;
    vfs_lock_acquire();
    int64_t result = may_read(node, space);
    struct vfs_xattr *attr = result == 0 ? find(node, name) : NULL;
    if (result == 0 && !attr) result = -ENODATA;
    if (result == 0) {
        result = (int64_t)attr->size;
        if (size && size < attr->size) result = -ERANGE;
        else if (size && attr->size) memcpy(out, attr->value, attr->size);
    }
    vfs_lock_release();
    return result;
}

int64_t vfs_xattr_set(struct vfs_node *node, const char *name, const void *value, size_t size,
                      int flags) {
    int space = known_namespace(name);
    if (space < 0) return -EOPNOTSUPP;
    if (strlen(name) > VFS_XATTR_NAME_MAX) return -ERANGE;
    if (flags & ~(XATTR_CREATE | XATTR_REPLACE)) return -EINVAL;
    vfs_lock_acquire();
    int64_t result = may_write(node, space);
    if (result == 0) {
        struct vfs_xattr *attr = find(node, name);
        if ((flags & XATTR_CREATE) && attr) result = -EEXIST;
        else if ((flags & XATTR_REPLACE) && !attr) result = -ENODATA;
        else if (total_bytes(node) - (attr ? strlen(name) + 1U + attr->size : 0) + strlen(name) +
                     1U + size >
                 VFS_XATTR_TOTAL_MAX)
            result = -ENOSPC;
    }
    if (result == 0) result = store(node, name, value, size);
    if (result == 0) {
        node->xattr_dirty = 1;
        vfs_stamp_times(node, VFS_TIME_CTIME);
    }
    vfs_lock_release();
    if (result == 0) vfs_notify_meta_changed(node);
    return result;
}

int64_t vfs_xattr_remove(struct vfs_node *node, const char *name) {
    int space = known_namespace(name);
    if (space < 0) return -EOPNOTSUPP;
    vfs_lock_acquire();
    int64_t result = may_write(node, space);
    struct vfs_xattr *attr = result == 0 ? find(node, name) : NULL;
    if (result == 0 && !attr) result = -ENODATA;
    if (result == 0) {
        unlink_attr(node, attr);
        node->xattr_dirty = 1;
        vfs_stamp_times(node, VFS_TIME_CTIME);
    }
    vfs_lock_release();
    if (result == 0) vfs_notify_meta_changed(node);
    return result;
}

int64_t vfs_xattr_list(struct vfs_node *node, char *out, size_t size) {
    vfs_lock_acquire();
    size_t total = 0;
    int64_t result = 0;
    for (struct vfs_xattr *attr = node->xattrs; attr; attr = attr->next) {
        if (may_read(node, known_namespace(attr->name)) != 0) continue;
        size_t length = strlen(attr->name) + 1U;
        if (size) {
            if (total + length > size) {
                result = -ERANGE;
                break;
            }
            memcpy(out + total, attr->name, length);
        }
        total += length;
    }
    vfs_lock_release();
    return result < 0 ? result : (int64_t)total;
}

int vfs_xattr_import(struct vfs_node *node, const char *name, const void *value, size_t size) {
    if (known_namespace(name) < 0 || strlen(name) > VFS_XATTR_NAME_MAX) return -EOPNOTSUPP;
    vfs_lock_acquire();
    int result = store(node, name, value, size);
    vfs_lock_release();
    return result;
}

void vfs_xattr_release(struct vfs_node *node) {
    struct vfs_xattr *attr = node->xattrs;
    node->xattrs = NULL;
    while (attr) {
        struct vfs_xattr *next = attr->next;
        kfree(attr->value);
        kfree(attr);
        attr = next;
    }
}

const struct vfs_xattr *vfs_xattr_first(const struct vfs_node *node) { return node->xattrs; }
