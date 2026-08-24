#include <stddef.h>
#include <stdint.h>
#include "include/cred.h"
#include "include/kstring.h"
#include "include/process.h"
#include "include/vfs.h"

#define EPERM 1
#define ENOENT 2
#define EACCES 13
#define EINVAL 22

#define MODE_SETUID 04000U
#define MODE_SETGID 02000U
#define MODE_STICKY 01000U

struct credentials *cred_current(void) {
    struct process *process = process_current();
    return process ? &process->cred : NULL;
}

int cred_is_root(void) {
    const struct credentials *cred = cred_current();
    return !cred || cred->euid == 0;
}

int cred_has_group(uint32_t gid) {
    const struct credentials *cred = cred_current();
    if (!cred) return 1;
    if (cred->egid == gid) return 1;
    for (uint32_t index = 0; index < cred->group_count; index++)
        if (cred->groups[index] == gid) return 1;
    return 0;
}

static int in_file_group(const struct credentials *cred, uint32_t gid) {
    if (cred->fsgid == gid) return 1;
    for (uint32_t index = 0; index < cred->group_count; index++)
        if (cred->groups[index] == gid) return 1;
    return 0;
}

int cred_owns(const struct vfs_node *node) {
    const struct credentials *cred = cred_current();
    if (!cred) return 1;
    return cred->fsuid == 0 || cred->fsuid == node->uid;
}

int cred_may(const struct vfs_node *node, uint32_t want) {
    if (!node) return -ENOENT;
    const struct credentials *cred = cred_current();
    if (!cred || !want) return 0;

    uint32_t allowed;
    if (cred->fsuid == node->uid) allowed = (node->mode >> 6) & 7U;
    else if (in_file_group(cred, node->gid)) allowed = (node->mode >> 3) & 7U;
    else allowed = node->mode & 7U;
    if ((allowed & want) == want) return 0;

    /* Root overrides the bits, except that a file with no execute bit at all
       is still not a program. */
    if (cred->fsuid == 0) {
        if (!(want & CRED_EXEC)) return 0;
        if ((node->flags & 0xFFU) == VFS_DIRECTORY) return 0;
        if (node->mode & 0111U) return 0;
    }
    return -EACCES;
}

int cred_may_search(const char *path) {
    const struct credentials *cred = cred_current();
    if (!cred || cred->fsuid == 0 || !path || path[0] != '/') return 0;

    char prefix[256];
    size_t length = strlen(path);
    if (length >= sizeof(prefix)) return -EACCES;
    memcpy(prefix, path, length + 1);

    for (size_t index = 1; index < length; index++) {
        if (prefix[index] != '/') continue;
        prefix[index] = '\0';
        struct vfs_node *directory = vfs_lookup(prefix);
        prefix[index] = '/';
        if (!directory) return -ENOENT;
        int status = cred_may(directory, CRED_EXEC);
        if (status != 0) return status;
    }
    return 0;
}

int cred_may_path(const char *path, const struct vfs_node *node, uint32_t want) {
    int status = cred_may_search(path);
    if (status != 0) return status;
    return cred_may(node, want);
}

/* The directory component of an absolute path, "/" when there is none. */
static struct vfs_node *parent_of(const char *path, char buffer[256]) {
    size_t length = strlen(path);
    if (length >= 256) return NULL;
    memcpy(buffer, path, length + 1);
    while (length > 1 && buffer[length - 1] != '/') length--;
    if (length > 1) length--;
    buffer[length ? length : 1] = '\0';
    return vfs_lookup(buffer);
}

int cred_may_write_parent(const char *path) {
    const struct credentials *cred = cred_current();
    if (!cred || cred->fsuid == 0) return 0;
    int status = cred_may_search(path);
    if (status != 0) return status;
    char buffer[256];
    struct vfs_node *parent = parent_of(path, buffer);
    if (!parent) return -ENOENT;
    return cred_may(parent, CRED_WRITE | CRED_EXEC);
}

int cred_may_remove(const char *path, const struct vfs_node *node) {
    int status = cred_may_write_parent(path);
    if (status != 0) return status;
    const struct credentials *cred = cred_current();
    if (!cred || cred->fsuid == 0) return 0;
    char buffer[256];
    struct vfs_node *parent = parent_of(path, buffer);
    if (!parent || !(parent->mode & MODE_STICKY)) return 0;
    if (cred->fsuid == node->uid || cred->fsuid == parent->uid) return 0;
    return -EPERM;
}

void cred_stamp_new_node(struct vfs_node *node) {
    const struct credentials *cred = cred_current();
    if (!node || !cred) return;
    node->uid = cred->fsuid;
    node->gid = cred->fsgid;
    if (node->parent && (node->parent->mode & MODE_SETGID)) {
        node->gid = node->parent->gid;
        if ((node->flags & 0xFFU) == VFS_DIRECTORY) node->mode |= MODE_SETGID;
    }
}

static int privileged(const struct credentials *cred) { return cred->euid == 0; }

static int one_of(uint32_t value, uint32_t a, uint32_t b, uint32_t c) {
    return value == a || value == b || value == c;
}

int64_t cred_set_uid(uint32_t uid) {
    struct credentials *cred = cred_current();
    if (!cred) return -EPERM;
    if (uid == CRED_UNCHANGED) return -EINVAL;
    if (privileged(cred)) {
        cred->uid = cred->euid = cred->suid = cred->fsuid = uid;
        return 0;
    }
    if (uid != cred->uid && uid != cred->suid) return -EPERM;
    cred->euid = cred->fsuid = uid;
    return 0;
}

int64_t cred_set_gid(uint32_t gid) {
    struct credentials *cred = cred_current();
    if (!cred) return -EPERM;
    if (gid == CRED_UNCHANGED) return -EINVAL;
    if (privileged(cred)) {
        cred->gid = cred->egid = cred->sgid = cred->fsgid = gid;
        return 0;
    }
    if (gid != cred->gid && gid != cred->sgid) return -EPERM;
    cred->egid = cred->fsgid = gid;
    return 0;
}

int64_t cred_set_reuid(uint32_t ruid, uint32_t euid) {
    struct credentials *cred = cred_current();
    if (!cred) return -EPERM;
    if (!privileged(cred)) {
        if (ruid != CRED_UNCHANGED && ruid != cred->uid && ruid != cred->euid)
            return -EPERM;
        if (euid != CRED_UNCHANGED &&
            !one_of(euid, cred->uid, cred->euid, cred->suid))
            return -EPERM;
    }
    uint32_t old_uid = cred->uid;
    if (ruid != CRED_UNCHANGED) cred->uid = ruid;
    if (euid != CRED_UNCHANGED) cred->euid = euid;
    if (ruid != CRED_UNCHANGED || (euid != CRED_UNCHANGED && euid != old_uid))
        cred->suid = cred->euid;
    cred->fsuid = cred->euid;
    return 0;
}

int64_t cred_set_regid(uint32_t rgid, uint32_t egid) {
    struct credentials *cred = cred_current();
    if (!cred) return -EPERM;
    if (!privileged(cred)) {
        if (rgid != CRED_UNCHANGED && rgid != cred->gid && rgid != cred->egid)
            return -EPERM;
        if (egid != CRED_UNCHANGED &&
            !one_of(egid, cred->gid, cred->egid, cred->sgid))
            return -EPERM;
    }
    uint32_t old_gid = cred->gid;
    if (rgid != CRED_UNCHANGED) cred->gid = rgid;
    if (egid != CRED_UNCHANGED) cred->egid = egid;
    if (rgid != CRED_UNCHANGED || (egid != CRED_UNCHANGED && egid != old_gid))
        cred->sgid = cred->egid;
    cred->fsgid = cred->egid;
    return 0;
}

int64_t cred_set_resuid(uint32_t ruid, uint32_t euid, uint32_t suid) {
    struct credentials *cred = cred_current();
    if (!cred) return -EPERM;
    if (!privileged(cred)) {
        const uint32_t values[3] = {ruid, euid, suid};
        for (int index = 0; index < 3; index++) {
            if (values[index] == CRED_UNCHANGED) continue;
            if (!one_of(values[index], cred->uid, cred->euid, cred->suid))
                return -EPERM;
        }
    }
    if (ruid != CRED_UNCHANGED) cred->uid = ruid;
    if (euid != CRED_UNCHANGED) cred->euid = euid;
    if (suid != CRED_UNCHANGED) cred->suid = suid;
    cred->fsuid = cred->euid;
    return 0;
}

int64_t cred_set_resgid(uint32_t rgid, uint32_t egid, uint32_t sgid) {
    struct credentials *cred = cred_current();
    if (!cred) return -EPERM;
    if (!privileged(cred)) {
        const uint32_t values[3] = {rgid, egid, sgid};
        for (int index = 0; index < 3; index++) {
            if (values[index] == CRED_UNCHANGED) continue;
            if (!one_of(values[index], cred->gid, cred->egid, cred->sgid))
                return -EPERM;
        }
    }
    if (rgid != CRED_UNCHANGED) cred->gid = rgid;
    if (egid != CRED_UNCHANGED) cred->egid = egid;
    if (sgid != CRED_UNCHANGED) cred->sgid = sgid;
    cred->fsgid = cred->egid;
    return 0;
}

/* setfsuid/setfsgid answer with the previous value and never with an error. */
int64_t cred_set_fsuid(uint32_t fsuid) {
    struct credentials *cred = cred_current();
    if (!cred) return 0;
    uint32_t previous = cred->fsuid;
    if (privileged(cred) || fsuid == cred->uid || fsuid == cred->euid ||
        fsuid == cred->suid || fsuid == cred->fsuid)
        cred->fsuid = fsuid;
    return previous;
}

int64_t cred_set_fsgid(uint32_t fsgid) {
    struct credentials *cred = cred_current();
    if (!cred) return 0;
    uint32_t previous = cred->fsgid;
    if (privileged(cred) || fsgid == cred->gid || fsgid == cred->egid ||
        fsgid == cred->sgid || fsgid == cred->fsgid)
        cred->fsgid = fsgid;
    return previous;
}

int64_t cred_set_groups(uint32_t count, const uint32_t *groups) {
    struct credentials *cred = cred_current();
    if (!cred) return -EPERM;
    if (count > CRED_MAX_GROUPS) return -EINVAL;
    if (!privileged(cred)) return -EPERM;
    for (uint32_t index = 0; index < count; index++) cred->groups[index] = groups[index];
    cred->group_count = count;
    return 0;
}

void cred_apply_exec(struct credentials *cred, const struct vfs_node *node,
                     int no_new_privs) {
    if (!cred) return;
    if (node && !no_new_privs) {
        if (node->mode & MODE_SETUID) cred->euid = node->uid;
        /* A group-executable bit is what separates set-group-ID from the
           mandatory-locking encoding, which shares the bit. */
        if ((node->mode & MODE_SETGID) && (node->mode & 010U)) cred->egid = node->gid;
    }
    cred->suid = cred->euid;
    cred->sgid = cred->egid;
    cred->fsuid = cred->euid;
    cred->fsgid = cred->egid;
}
