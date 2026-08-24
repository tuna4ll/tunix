#ifndef TUNIX_CRED_H
#define TUNIX_CRED_H

#include <stdint.h>

/*
 * Process credentials and the file permission checks built on them.
 *
 * Tunix used to have exactly one identity: every process was root and every
 * path check was "does the node exist". Everything here exists so that a
 * process can be somebody else, and so that being somebody else is enforced.
 *
 * The model is the POSIX one, without capabilities: euid 0 is privileged, the
 * saved ids make a setuid program able to drop and regain its identity, and the
 * fsuid/fsgid pair is what file access is actually judged against.
 */

#define CRED_MAX_GROUPS 32
/* uid_t is 32 bits, so "leave this one alone" arrives as 0xFFFFFFFF. */
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
    uint32_t groups[CRED_MAX_GROUPS];
};

struct credentials *cred_current(void);
int cred_is_root(void);
int cred_has_group(uint32_t gid);

/* 0 when the access is allowed, -EACCES when it is not. */
int cred_may(const struct vfs_node *node, uint32_t want);
/* Execute permission on every directory leading to an absolute path. */
int cred_may_search(const char *path);
/* Search the leading directories, then check `want` on the node itself. */
int cred_may_path(const char *path, const struct vfs_node *node, uint32_t want);
/* Search and write permission on the directory holding `path`. */
int cred_may_write_parent(const char *path);
/* The sticky-bit rule: in a +t directory only the owner may unlink. */
int cred_may_remove(const char *path, const struct vfs_node *node);
int cred_owns(const struct vfs_node *node);
/* Ownership for a node the current process is creating. */
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

/* The set-user-ID / set-group-ID transition an exec of `node` performs. */
void cred_apply_exec(struct credentials *cred, const struct vfs_node *node,
                     int no_new_privs);

#endif
