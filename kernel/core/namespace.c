#include <stddef.h>
#include <stdint.h>
#include <tunix/cred.h>
#include <tunix/errno.h>
#include <tunix/heap.h>
#include <tunix/kstring.h>
#include <tunix/namespace.h>
#include <tunix/net/net.h>
#include <tunix/process.h>
#include <tunix/uts.h>

static struct user_namespace root_user_ns = {.refs = 1, .level = 0};
static struct uts_namespace root_uts_ns = {.refs = 1};
static struct pid_namespace root_pid_ns = {.refs = 1, .level = 0};
static struct mount_namespace root_mnt_ns = {.refs = 1};

static struct mount_namespace *mnt_ns_of(const struct process *process) {
    return process && process->mnt_ns ? process->mnt_ns : &root_mnt_ns;
}

int mnt_ns_active(void) {
    struct process *process = process_current();
    return process && process->mnt_ns != NULL;
}

struct vfs_node *mnt_ns_resolve(struct vfs_node *at) {
    struct process *process = process_current();
    if (!process || !process->mnt_ns) return NULL;
    for (struct mount_override *o = process->mnt_ns->overrides; o; o = o->next)
        if (o->at == at) return o->masked ? at : o->root;
    return NULL;
}

int mnt_ns_add(struct vfs_node *at, struct vfs_node *root) {
    struct process *process = process_current();
    if (!process || !process->mnt_ns) return -1;
    struct mount_namespace *ns = process->mnt_ns;
    for (struct mount_override *o = ns->overrides; o; o = o->next)
        if (o->at == at) {
            o->root = root;
            o->masked = 0;
            return 0;
        }
    struct mount_override *entry = kmalloc(sizeof(*entry));
    if (!entry) return -1;
    entry->at = at;
    entry->root = root;
    entry->masked = 0;
    entry->next = ns->overrides;
    ns->overrides = entry;
    return 0;
}

int mnt_ns_remove(struct vfs_node *at) {
    struct process *process = process_current();
    if (!process || !process->mnt_ns) return -1;
    for (struct mount_override *o = process->mnt_ns->overrides; o; o = o->next)
        if (o->at == at) {
            o->masked = 1;
            o->root = NULL;
            return 0;
        }
    struct mount_override *entry = kmalloc(sizeof(*entry));
    if (!entry) return -1;
    entry->at = at;
    entry->root = NULL;
    entry->masked = 1;
    entry->next = process->mnt_ns->overrides;
    process->mnt_ns->overrides = entry;
    return 0;
}

static struct mount_namespace *new_mnt_ns(struct process *process) {
    struct mount_namespace *parent = mnt_ns_of(process);
    struct mount_namespace *ns = kmalloc(sizeof(*ns));
    if (!ns) return NULL;
    memset(ns, 0, sizeof(*ns));
    ns->refs = 1;
    ns->parent = parent;
    for (struct mount_override *o = parent->overrides; o; o = o->next) {
        struct mount_override *copy = kmalloc(sizeof(*copy));
        if (!copy) continue;
        *copy = *o;
        copy->next = ns->overrides;
        ns->overrides = copy;
    }
    __atomic_add_fetch(&parent->refs, 1, __ATOMIC_RELAXED);
    return ns;
}

static void mnt_ns_put(struct mount_namespace *ns) {
    while (ns && ns != &root_mnt_ns) {
        if (__atomic_sub_fetch(&ns->refs, 1, __ATOMIC_ACQ_REL) != 0) return;
        struct mount_override *o = ns->overrides;
        while (o) {
            struct mount_override *next = o->next;
            kfree(o);
            o = next;
        }
        struct mount_namespace *parent = ns->parent;
        kfree(ns);
        ns = parent;
    }
}

void namespace_assign_pid(struct process *child, const struct process *parent, int is_thread) {
    if (!child->pid_ns) {
        child->ns_pid = child->pid;
        child->ns_tgid = child->tgid;
        return;
    }
    child->ns_pid = pid_ns_alloc(child->pid_ns);
    child->ns_tgid = is_thread && parent ? parent->ns_tgid : child->ns_pid;
}

uint64_t pid_ns_alloc(struct pid_namespace *ns) {
    if (!ns || ns == &root_pid_ns) return 0;
    return __atomic_add_fetch(&ns->last_pid, 1, __ATOMIC_RELAXED);
}

static struct pid_namespace *pid_ns_of(const struct process *process) {
    return process && process->pid_ns ? process->pid_ns : &root_pid_ns;
}

static struct pid_namespace *new_pid_ns(struct process *process) {
    struct pid_namespace *parent = pid_ns_of(process);
    if (parent->level >= USER_NS_MAX_DEPTH) return NULL;
    struct pid_namespace *ns = kmalloc(sizeof(*ns));
    if (!ns) return NULL;
    memset(ns, 0, sizeof(*ns));
    ns->parent = parent;
    ns->refs = 1;
    ns->level = parent->level + 1U;
    __atomic_add_fetch(&parent->refs, 1, __ATOMIC_RELAXED);
    return ns;
}

static void pid_ns_put(struct pid_namespace *ns) {
    while (ns && ns != &root_pid_ns) {
        if (__atomic_sub_fetch(&ns->refs, 1, __ATOMIC_ACQ_REL) != 0) return;
        struct pid_namespace *parent = ns->parent;
        kfree(ns);
        ns = parent;
    }
}

int uts_ns_is_root(const struct uts_namespace *ns) { return !ns || ns == &root_uts_ns; }

static struct user_namespace *user_ns_of(const struct process *process) {
    return process && process->user_ns ? process->user_ns : &root_user_ns;
}

struct uts_namespace *uts_ns_current(void) {
    struct process *process = process_current();
    return process && process->uts_ns ? process->uts_ns : &root_uts_ns;
}

static uint32_t map_up(const struct uid_map *map, uint32_t lower) {
    for (uint32_t index = 0; index < map->extents; index++) {
        const struct uid_extent *extent = &map->extent[index];
        if (lower >= extent->lower_first && lower - extent->lower_first < extent->count)
            return extent->first + (lower - extent->lower_first);
    }
    return 65534U;
}

uint32_t user_ns_map_uid(const struct user_namespace *ns, uint32_t kuid) {
    if (!ns || ns->level == 0 || !ns->uid_map_set) return kuid;
    return map_up(&ns->uid, kuid);
}

uint32_t user_ns_map_gid(const struct user_namespace *ns, uint32_t kgid) {
    if (!ns || ns->level == 0 || !ns->gid_map_set) return kgid;
    return map_up(&ns->gid, kgid);
}

int user_ns_capable(void) {
    struct process *process = process_current();
    if (!process) return 1;
    if (process->cred.euid == 0 && user_ns_of(process)->level == 0) return 1;
    return user_ns_of(process)->level > 0;
}

void namespace_inherit(struct process *child, const struct process *parent) {
    struct user_namespace *user = user_ns_of(parent);
    struct uts_namespace *uts = parent && parent->uts_ns ? parent->uts_ns : &root_uts_ns;
    child->user_ns = user;
    child->uts_ns = uts;
    child->net_ns = parent ? parent->net_ns : NULL;
    child->pid_ns = parent ? parent->pid_ns : NULL;
    child->mnt_ns = parent ? parent->mnt_ns : NULL;
    __atomic_add_fetch(&user->refs, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&uts->refs, 1, __ATOMIC_RELAXED);
    net_ns_get(child->net_ns);
    if (child->pid_ns) __atomic_add_fetch(&child->pid_ns->refs, 1, __ATOMIC_RELAXED);
    if (child->mnt_ns) __atomic_add_fetch(&child->mnt_ns->refs, 1, __ATOMIC_RELAXED);
}

static void user_ns_put(struct user_namespace *ns) {
    while (ns && ns != &root_user_ns) {
        if (__atomic_sub_fetch(&ns->refs, 1, __ATOMIC_ACQ_REL) != 0) return;
        struct user_namespace *parent = ns->parent;
        kfree(ns);
        ns = parent;
    }
}

static void uts_ns_put(struct uts_namespace *ns) {
    if (!ns || ns == &root_uts_ns) return;
    if (__atomic_sub_fetch(&ns->refs, 1, __ATOMIC_ACQ_REL) != 0) return;
    struct uts_namespace *parent = ns->parent;
    kfree(ns);
    uts_ns_put(parent);
}

void namespace_release(struct process *process) {
    struct user_namespace *user = process->user_ns;
    struct uts_namespace *uts = process->uts_ns;
    process->user_ns = NULL;
    process->uts_ns = NULL;
    net_ns_put(process->net_ns);
    process->net_ns = NULL;
    pid_ns_put(process->pid_ns);
    process->pid_ns = NULL;
    mnt_ns_put(process->mnt_ns);
    process->mnt_ns = NULL;
    user_ns_put(user);
    uts_ns_put(uts);
}

static struct user_namespace *new_user_ns(struct process *process) {
    struct user_namespace *parent = user_ns_of(process);
    if (parent->level >= USER_NS_MAX_DEPTH) return NULL;
    struct user_namespace *ns = kmalloc(sizeof(*ns));
    if (!ns) return NULL;
    memset(ns, 0, sizeof(*ns));
    ns->parent = parent;
    ns->refs = 1;
    ns->level = parent->level + 1U;
    ns->owner_uid = process->cred.euid;
    ns->owner_gid = process->cred.egid;
    __atomic_add_fetch(&parent->refs, 1, __ATOMIC_RELAXED);
    return ns;
}

static struct uts_namespace *new_uts_ns(struct process *process) {
    struct uts_namespace *parent = process->uts_ns ? process->uts_ns : &root_uts_ns;
    struct uts_namespace *ns = kmalloc(sizeof(*ns));
    if (!ns) return NULL;
    memset(ns, 0, sizeof(*ns));
    ns->refs = 1;
    ns->parent = parent;
    if (parent == &root_uts_ns) {
        strncpy(ns->nodename, uts_hostname(), UTS_FIELD_MAX - 1);
        strncpy(ns->domainname, uts_domainname(), UTS_FIELD_MAX - 1);
    } else {
        memcpy(ns->nodename, parent->nodename, UTS_FIELD_MAX);
        memcpy(ns->domainname, parent->domainname, UTS_FIELD_MAX);
    }
    __atomic_add_fetch(&parent->refs, 1, __ATOMIC_RELAXED);
    return ns;
}

static int64_t apply_flags(struct process *process, uint64_t flags) {
    uint64_t supported = CLONE_NEWUSER | CLONE_NEWUTS | CLONE_NEWNET | CLONE_NEWPID | CLONE_NEWNS;
    if (flags & CLONE_NAMESPACE_FLAGS & ~supported) return -EINVAL;

    if (!(flags & CLONE_NEWUSER)) {
        if ((flags & (CLONE_NEWUTS | CLONE_NEWNET | CLONE_NEWPID | CLONE_NEWNS)) &&
            !user_ns_capable())
            return -EPERM;
    }

    struct user_namespace *created_user = NULL;
    struct uts_namespace *created_uts = NULL;
    if (flags & CLONE_NEWUSER) {
        created_user = new_user_ns(process);
        if (!created_user) return -ENOMEM;
    }
    if (flags & CLONE_NEWUTS) {
        created_uts = new_uts_ns(process);
        if (!created_uts) {
            user_ns_put(created_user);
            return -ENOMEM;
        }
    }
    struct net_namespace *created_net = NULL;
    if (flags & CLONE_NEWNET) {
        created_net = net_ns_create();
        if (!created_net) {
            user_ns_put(created_user);
            uts_ns_put(created_uts);
            return -ENOMEM;
        }
    }
    struct pid_namespace *created_pid = NULL;
    if (flags & CLONE_NEWPID) {
        created_pid = new_pid_ns(process);
        if (!created_pid) {
            user_ns_put(created_user);
            uts_ns_put(created_uts);
            net_ns_put(created_net);
            return -ENOMEM;
        }
    }
    struct mount_namespace *created_mnt = NULL;
    if (flags & CLONE_NEWNS) {
        created_mnt = new_mnt_ns(process);
        if (!created_mnt) {
            user_ns_put(created_user);
            uts_ns_put(created_uts);
            net_ns_put(created_net);
            pid_ns_put(created_pid);
            return -ENOMEM;
        }
    }
    if (created_user) {
        user_ns_put(process->user_ns);
        process->user_ns = created_user;
    }
    if (created_uts) {
        uts_ns_put(process->uts_ns);
        process->uts_ns = created_uts;
    }
    if (created_net) {
        net_ns_put(process->net_ns);
        process->net_ns = created_net;
    }
    if (created_pid) {
        pid_ns_put(process->pid_ns);
        process->pid_ns = created_pid;
    }
    if (created_mnt) {
        mnt_ns_put(process->mnt_ns);
        process->mnt_ns = created_mnt;
    }
    return 0;
}

int64_t namespace_apply_clone(struct process *child, uint64_t flags) {
    return apply_flags(child, flags);
}

int64_t namespace_unshare(uint64_t flags) {
    struct process *process = process_current();
    if (!process) return -EINVAL;
    uint64_t handled = CLONE_NAMESPACE_FLAGS & ~CLONE_NEWPID;
    if (flags & ~handled) return -EINVAL;
    return apply_flags(process, flags);
}

static int parse_u32(const char **cursor, const char *end, uint32_t *out) {
    while (*cursor < end && (**cursor == ' ' || **cursor == '\t')) (*cursor)++;
    if (*cursor >= end || **cursor < '0' || **cursor > '9') return -1;
    uint64_t value = 0;
    while (*cursor < end && **cursor >= '0' && **cursor <= '9') {
        value = value * 10U + (uint64_t)(**cursor - '0');
        if (value > 0xFFFFFFFFULL) return -1;
        (*cursor)++;
    }
    *out = (uint32_t)value;
    return 0;
}

static int64_t write_map(struct uid_map *map, int *flag, const char *text, uint64_t length) {
    struct process *process = process_current();
    struct user_namespace *ns = user_ns_of(process);
    if (ns->level == 0 || *flag) return -EPERM;
    struct uid_map parsed;
    parsed.extents = 0;
    const char *cursor = text;
    const char *end = text + length;
    while (cursor < end) {
        while (cursor < end && (*cursor == '\n' || *cursor == ' ' || *cursor == '\t')) cursor++;
        if (cursor >= end) break;
        if (parsed.extents >= UID_MAP_MAX) return -EINVAL;
        struct uid_extent *extent = &parsed.extent[parsed.extents];
        if (parse_u32(&cursor, end, &extent->first) != 0) return -EINVAL;
        if (parse_u32(&cursor, end, &extent->lower_first) != 0) return -EINVAL;
        if (parse_u32(&cursor, end, &extent->count) != 0 || extent->count == 0) return -EINVAL;
        if (extent->first + extent->count < extent->first ||
            extent->lower_first + extent->count < extent->lower_first)
            return -EINVAL;
        parsed.extents++;
    }
    if (parsed.extents == 0) return -EINVAL;
    *map = parsed;
    *flag = 1;
    return (int64_t)length;
}

int64_t user_ns_write_uid_map(int is_gid, const char *text, uint64_t length) {
    struct process *process = process_current();
    struct user_namespace *ns = user_ns_of(process);
    if (is_gid && !ns->setgroups_allowed && ns->gid_map_set) return -EPERM;
    return is_gid ? write_map(&ns->gid, &ns->gid_map_set, text, length)
                  : write_map(&ns->uid, &ns->uid_map_set, text, length);
}

int64_t user_ns_write_setgroups(const char *text, uint64_t length) {
    struct process *process = process_current();
    struct user_namespace *ns = user_ns_of(process);
    if (ns->level == 0) return -EPERM;
    if (length >= 4 && memcmp(text, "deny", 4) == 0) {
        if (ns->gid_map_set) return -EPERM;
        ns->setgroups_allowed = 0;
    } else if (length >= 5 && memcmp(text, "allow", 5) == 0) {
        ns->setgroups_allowed = 1;
    } else {
        return -EINVAL;
    }
    return (int64_t)length;
}

int64_t user_ns_read_uid_map(int is_gid, char *out, uint64_t capacity) {
    struct process *process = process_current();
    struct user_namespace *ns = user_ns_of(process);
    const struct uid_map *map = is_gid ? &ns->gid : &ns->uid;
    uint64_t used = 0;
    for (uint32_t index = 0; index < map->extents; index++) {
        const struct uid_extent *extent = &map->extent[index];
        char line[64];
        unsigned at = 0;
        uint32_t values[3] = {extent->first, extent->lower_first, extent->count};
        for (unsigned v = 0; v < 3; v++) {
            if (v) line[at++] = ' ';
            char digits[11];
            unsigned count = 0;
            uint32_t number = values[v];
            do {
                digits[count++] = (char)('0' + number % 10U);
                number /= 10U;
            } while (number);
            while (count) line[at++] = digits[--count];
        }
        line[at++] = '\n';
        for (unsigned i = 0; i < at && used < capacity; i++) out[used++] = line[i];
    }
    return (int64_t)used;
}
