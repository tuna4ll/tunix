#ifndef TUNIX_CRED_H
#define TUNIX_CRED_H

#include <stdint.h>

#define CRED_MAX_GROUPS 65536

#define CRED_UNCHANGED ((uint32_t)0xFFFFFFFFU)

#define CRED_EXEC  1U
#define CRED_WRITE 2U
#define CRED_READ  4U

struct vfs_node;

struct credentials {
    uint32_t uid, gid;
    uint32_t euid, egid;
    uint32_t suid, sgid;
    uint32_t fsuid, fsgid;
    uint32_t group_count;
    uint32_t *groups;
};

void cred_groups_share(struct credentials *cred);
void cred_groups_release(struct credentials *cred);

struct credentials *cred_current(void);
int cred_is_root(void);
int cred_has_group(uint32_t gid);

int cred_may(const struct vfs_node *node, uint32_t want);

int cred_may_search(const char *path);

int cred_may_path(const char *path, const struct vfs_node *node, uint32_t want);

int cred_may_write_parent(const char *path);

int cred_may_remove(const char *path, const struct vfs_node *node);
int cred_owns(const struct vfs_node *node);

void cred_stamp_new_node(struct vfs_node *node);

int64_t cred_set_uid(uint32_t uid);
int64_t cred_set_gid(uint32_t gid);
int64_t cred_set_reuid(uint32_t ruid, uint32_t euid);
int64_t cred_set_regid(uint32_t rgid, uint32_t egid);
int64_t cred_set_resuid(uint32_t ruid, uint32_t euid, uint32_t suid);
int64_t cred_set_resgid(uint32_t rgid, uint32_t egid, uint32_t sgid);
int64_t cred_set_fsuid(uint32_t fsuid);
int64_t cred_set_fsgid(uint32_t fsgid);
int64_t cred_set_groups(uint32_t count, const uint32_t *groups);

void cred_apply_exec(struct credentials *cred, const struct vfs_node *node,
                     int no_new_privs);

#endif
