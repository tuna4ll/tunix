#include <stddef.h>
#include <stdint.h>
#include <tunix/acl.h>
#include <tunix/cred.h>
#include <tunix/defer.h>
#include <tunix/kstring.h>
#include <tunix/vfs.h>

#define EPERM 1
#define EEXIST 17
#define EINVAL 22
#define EROFS 30
#define ERANGE 34
#define ENODATA 61
#define EOPNOTSUPP 95

#define ACL_VERSION 2U
#define ACL_USER_OBJ 0x01U
#define ACL_USER 0x02U
#define ACL_GROUP_OBJ 0x04U
#define ACL_GROUP 0x08U
#define ACL_MASK 0x10U
#define ACL_OTHER 0x20U
#define ACL_MAX_ENTRIES 64U
#define XATTR_CREATE 1
#define XATTR_REPLACE 2

struct acl_entry {
    uint16_t tag;
    uint16_t perm;
    uint32_t id;
};

struct vfs_acl {
    uint32_t group_obj;
    uint32_t count;
    struct acl_entry entries[];
};

static int is_access(const char *name) { return strcmp(name, ACL_XATTR_ACCESS) == 0; }
static int is_default(const char *name) { return strcmp(name, ACL_XATTR_DEFAULT) == 0; }

static struct vfs_acl *acl_of(const struct vfs_node *node) {
    return __atomic_load_n((struct vfs_acl *const *)&node->acl, __ATOMIC_ACQUIRE);
}

static void acl_swap(struct vfs_node *node, struct vfs_acl *next) {
    struct vfs_acl *old = __atomic_exchange_n((struct vfs_acl **)&node->acl, next, __ATOMIC_ACQ_REL);
    if (old) defer_free(old);
}

static int may_change(const struct vfs_node *node) {
    if (node->flags & VFS_READONLY) return -EROFS;
    if (node->disk_inode) return -EOPNOTSUPP;
    const struct credentials *cred = cred_current();
    if (cred && cred->fsuid != 0 && cred->fsuid != node->uid) return -EPERM;
    return 0;
}

static void put_entry(uint8_t *out, size_t *at, uint16_t tag, uint16_t perm, uint32_t id) {
    struct acl_entry entry = { tag, perm, id };
    memcpy(out + *at, &entry, sizeof(entry));
    *at += sizeof(entry);
}

int64_t acl_xattr_get(struct vfs_node *node, const char *name, void *out, size_t size) {
    if (!is_access(name)) return -ENODATA;
    struct vfs_acl *acl = acl_of(node);
    if (!acl) return -ENODATA;
    size_t length = 4U + (acl->count + 4U) * sizeof(struct acl_entry);
    if (!size) return (int64_t)length;
    if (size < length) return -ERANGE;
    uint8_t *bytes = (uint8_t *)out;
    uint32_t version = ACL_VERSION;
    memcpy(bytes, &version, sizeof(version));
    size_t at = 4;
    uint32_t mode = node->mode;
    put_entry(bytes, &at, ACL_USER_OBJ, (uint16_t)((mode >> 6) & 7U), UINT32_MAX);
    for (uint32_t index = 0; index < acl->count; index++)
        if (acl->entries[index].tag == ACL_USER)
            put_entry(bytes, &at, ACL_USER, acl->entries[index].perm, acl->entries[index].id);
    put_entry(bytes, &at, ACL_GROUP_OBJ, (uint16_t)acl->group_obj, UINT32_MAX);
    for (uint32_t index = 0; index < acl->count; index++)
        if (acl->entries[index].tag == ACL_GROUP)
            put_entry(bytes, &at, ACL_GROUP, acl->entries[index].perm, acl->entries[index].id);
    put_entry(bytes, &at, ACL_MASK, (uint16_t)((mode >> 3) & 7U), UINT32_MAX);
    put_entry(bytes, &at, ACL_OTHER, (uint16_t)(mode & 7U), UINT32_MAX);
    return (int64_t)length;
}

static int entry_before(const struct acl_entry *a, const struct acl_entry *b) {
    if (a->tag != b->tag) return a->tag < b->tag;
    return a->id < b->id;
}

int64_t acl_xattr_set(struct vfs_node *node, const char *name, const void *value, size_t size,
                      int flags) {
    if (is_default(name)) return -EOPNOTSUPP;
    if (!is_access(name)) return -EOPNOTSUPP;
    int allowed = may_change(node);
    if (allowed != 0) return allowed;
    if (flags & ~(XATTR_CREATE | XATTR_REPLACE)) return -EINVAL;
    if ((flags & XATTR_CREATE) && acl_of(node)) return -EEXIST;
    if ((flags & XATTR_REPLACE) && !acl_of(node)) return -ENODATA;
    if (!value || size < 4U || (size - 4U) % sizeof(struct acl_entry)) return -EINVAL;
    uint32_t version;
    memcpy(&version, value, sizeof(version));
    if (version != ACL_VERSION) return -EINVAL;
    uint32_t total = (uint32_t)((size - 4U) / sizeof(struct acl_entry));
    if (total < 3U || total > ACL_MAX_ENTRIES) return -EINVAL;

    struct acl_entry entries[ACL_MAX_ENTRIES];
    memcpy(entries, (const uint8_t *)value + 4, total * sizeof(struct acl_entry));
    int user_obj = -1, group_obj = -1, mask = -1, other = -1;
    uint32_t named = 0;
    for (uint32_t index = 0; index < total; index++) {
        struct acl_entry *entry = &entries[index];
        if (entry->perm > 7U) return -EINVAL;
        switch (entry->tag) {
        case ACL_USER_OBJ: if (user_obj >= 0) return -EINVAL; user_obj = entry->perm; break;
        case ACL_GROUP_OBJ: if (group_obj >= 0) return -EINVAL; group_obj = entry->perm; break;
        case ACL_MASK: if (mask >= 0) return -EINVAL; mask = entry->perm; break;
        case ACL_OTHER: if (other >= 0) return -EINVAL; other = entry->perm; break;
        case ACL_USER: case ACL_GROUP: entries[named++] = *entry; break;
        default: return -EINVAL;
        }
    }
    if (user_obj < 0 || group_obj < 0 || other < 0) return -EINVAL;
    if (named && mask < 0) return -EINVAL;
    for (uint32_t index = 1; index < named; index++) {
        struct acl_entry key = entries[index];
        uint32_t at = index;
        while (at && entry_before(&key, &entries[at - 1])) {
            entries[at] = entries[at - 1];
            at--;
        }
        entries[at] = key;
    }
    for (uint32_t index = 1; index < named; index++)
        if (entries[index].tag == entries[index - 1].tag && entries[index].id == entries[index - 1].id)
            return -EINVAL;

    struct vfs_acl *next = NULL;
    if (named) {
        next = (struct vfs_acl *)defer_alloc(sizeof(*next) + named * sizeof(struct acl_entry));
        if (!next) return -12;
        next->group_obj = (uint32_t)group_obj;
        next->count = named;
        memcpy(next->entries, entries, named * sizeof(struct acl_entry));
    }
    uint32_t group_bits = (uint32_t)(named ? mask : group_obj);
    node->mode = (node->mode & ~0777U) | ((uint32_t)user_obj << 6) | (group_bits << 3) |
                 (uint32_t)other;
    acl_swap(node, next);
    vfs_stamp_times(node, VFS_TIME_CTIME);
    return 0;
}

int64_t acl_xattr_remove(struct vfs_node *node, const char *name) {
    if (!is_access(name) && !is_default(name)) return -ENODATA;
    if (is_default(name) || !acl_of(node)) return -ENODATA;
    int allowed = may_change(node);
    if (allowed != 0) return allowed;
    acl_swap(node, NULL);
    vfs_stamp_times(node, VFS_TIME_CTIME);
    return 0;
}

int64_t acl_xattr_list(struct vfs_node *node, char *out, size_t size) {
    if (!acl_of(node)) return 0;
    size_t length = strlen(ACL_XATTR_ACCESS) + 1U;
    if (!size) return (int64_t)length;
    if (size < length) return -ERANGE;
    memcpy(out, ACL_XATTR_ACCESS, length);
    return (int64_t)length;
}

static int in_group(const struct credentials *cred, uint32_t gid) {
    if (cred->fsgid == gid) return 1;
    for (uint32_t index = 0; index < cred->group_count; index++)
        if (cred->groups[index] == gid) return 1;
    return 0;
}

int acl_permission(const struct vfs_node *node, const struct credentials *cred, uint32_t want,
                   int *decided) {
    *decided = 0;
    struct vfs_acl *acl = acl_of(node);
    if (!acl || cred->fsuid == node->uid) return 0;
    uint32_t mask = (node->mode >> 3) & 7U;
    *decided = 1;
    for (uint32_t index = 0; index < acl->count; index++) {
        const struct acl_entry *entry = &acl->entries[index];
        if (entry->tag == ACL_USER && entry->id == cred->fsuid)
            return ((entry->perm & mask) & want) == want;
    }
    int matched = 0;
    if (in_group(cred, node->gid)) {
        matched = 1;
        if (((acl->group_obj & mask) & want) == want) return 1;
    }
    for (uint32_t index = 0; index < acl->count; index++) {
        const struct acl_entry *entry = &acl->entries[index];
        if (entry->tag != ACL_GROUP || !in_group(cred, entry->id)) continue;
        matched = 1;
        if (((entry->perm & mask) & want) == want) return 1;
    }
    if (matched) return 0;
    return ((node->mode & 7U) & want) == want;
}

void acl_release(struct vfs_node *node) {
    struct vfs_acl *acl = __atomic_exchange_n((struct vfs_acl **)&node->acl, NULL, __ATOMIC_ACQ_REL);
    if (acl) defer_free(acl);
}
