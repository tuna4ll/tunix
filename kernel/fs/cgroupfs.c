#include <stddef.h>
#include <stdint.h>
#include <tunix/cgroup.h>
#include <tunix/heap.h>
#include <tunix/inotify.h>
#include <tunix/kstring.h>
#include <tunix/lock.h>
#include <tunix/process.h>
#include <tunix/vfs.h>

#define EPERM 1
#define ENOENT 2
#define ESRCH 3
#define ENOMEM 12
#define EBUSY 16
#define EINVAL 22
#define ENOTEMPTY 39
#define SIGKILL 9

enum {
    FILE_PROCS,
    FILE_THREADS,
    FILE_TASKS,
    FILE_CONTROLLERS,
    FILE_SUBTREE,
    FILE_TYPE,
    FILE_EVENTS,
    FILE_STAT,
    FILE_FREEZE,
    FILE_KILL,
    FILE_MAX_DEPTH,
    FILE_MAX_DESCENDANTS,
    FILE_NOTIFY,
    FILE_CLONE_CHILDREN,
    FILE_RELEASE_AGENT,
    FILE_SANE,
    FILE_COUNT
};

struct cgroup_file {
    struct cgroup *cgroup;
    unsigned kind;
};

struct hierarchy {
    int used;
    int v2;
    char name[32];
    struct cgroup *root;
    struct vfs_node *mount_root;
    char release_agent[128];
};

struct cgroup {
    struct hierarchy *hierarchy;
    unsigned index;
    struct cgroup *parent;
    struct cgroup *children;
    struct cgroup *sibling;
    struct vfs_node *dir;
    struct vfs_node *events;
    uint32_t tasks;
    uint32_t refs;
    uint8_t notify_on_release;
    uint8_t clone_children;
    uint8_t frozen;
    struct cgroup_file files[FILE_COUNT];
};

static struct cgroup default_root = {.refs = 1};
static struct hierarchy hierarchies[CGROUP_HIERARCHIES] = {
    {.used = 1, .v2 = 1, .root = &default_root},
};
static struct lock cgroup_lock = LOCK_INITIALIZER("cgroup", LOCK_RANK_REGISTRY);

static void cgroup_guard_release(int *unused) {
    (void)unused;
    lock_release(&cgroup_lock);
}

#define CGROUP_LOCKED \
    __attribute__((cleanup(cgroup_guard_release))) int cgroup_guard = (lock_acquire(&cgroup_lock), 0)

struct text {
    char *data;
    size_t length;
    size_t capacity;
};

static void text_free(struct text *text) {
    kfree(text->data);
}

static void text_put(struct text *text, const char *value, size_t length) {
    if (text->length + length + 1 > text->capacity) {
        size_t capacity = text->capacity ? text->capacity : 256;
        while (capacity < text->length + length + 1) capacity *= 2;
        char *grown = (char *)kmalloc(capacity);
        if (!grown) return;
        if (text->length) memcpy(grown, text->data, text->length);
        kfree(text->data);
        text->data = grown;
        text->capacity = capacity;
    }
    memcpy(text->data + text->length, value, length);
    text->length += length;
    text->data[text->length] = '\0';
}

static void text_string(struct text *text, const char *value) {
    text_put(text, value, strlen(value));
}

static void text_number(struct text *text, uint64_t value) {
    char digits[24];
    int count = 0;
    do {
        digits[count++] = (char)('0' + value % 10U);
        value /= 10U;
    } while (value);
    char out[24];
    for (int index = 0; index < count; index++) out[index] = digits[count - 1 - index];
    text_put(text, out, (size_t)count);
}

static int64_t text_reply(struct text *text, uint64_t offset, size_t size, void *output) {
    int64_t moved = 0;
    if (offset < text->length) {
        size_t available = text->length - (size_t)offset;
        if (size > available) size = available;
        memcpy(output, text->data + offset, size);
        moved = (int64_t)size;
    }
    text_free(text);
    return moved;
}

static unsigned hierarchy_index(const struct hierarchy *hierarchy) {
    return (unsigned)(hierarchy - hierarchies);
}

static struct cgroup *member_of(struct process *process, unsigned index) {
    struct cgroup *cgroup = process->cgroups[index];
    return cgroup ? cgroup : hierarchies[index].root;
}

static int descends_from(const struct cgroup *cgroup, const struct cgroup *ancestor) {
    for (; cgroup; cgroup = cgroup->parent)
        if (cgroup == ancestor) return 1;
    return 0;
}

static int subtree_populated(const struct cgroup *cgroup) {
    if (cgroup->tasks) return 1;
    for (const struct cgroup *child = cgroup->children; child; child = child->sibling)
        if (subtree_populated(child)) return 1;
    return 0;
}

static uint32_t descendant_count(const struct cgroup *cgroup) {
    uint32_t count = 0;
    for (const struct cgroup *child = cgroup->children; child; child = child->sibling)
        count += 1U + descendant_count(child);
    return count;
}

static void cgroup_put(struct cgroup *cgroup) {
    int last;
    {
        CGROUP_LOCKED;
        last = --cgroup->refs == 0;
    }
    if (last && cgroup != &default_root) kfree(cgroup);
}

static void notify_events(struct cgroup **changed, unsigned count) {
    for (unsigned index = 0; index < count; index++) {
        if (changed[index]->events)
            inotify_notify(changed[index]->events, TUNIX_IN_MODIFY, NULL, 0);
        cgroup_put(changed[index]);
    }
}

static unsigned collect_populated(struct cgroup *cgroup, int was[], struct cgroup **out,
                                  unsigned limit) {
    unsigned count = 0;
    for (unsigned depth = 0; cgroup && count < limit; cgroup = cgroup->parent, depth++) {
        if (depth < limit && was[depth] != subtree_populated(cgroup)) {
            cgroup->refs++;
            out[count++] = cgroup;
        }
    }
    return count;
}

static void remember_populated(struct cgroup *cgroup, int was[], unsigned limit) {
    for (unsigned depth = 0; depth < limit; depth++) {
        was[depth] = cgroup ? subtree_populated(cgroup) : 0;
        if (cgroup) cgroup = cgroup->parent;
    }
}

#define NOTIFY_DEPTH 16

void cgroup_fork(struct process *parent, struct process *child) {
    CGROUP_LOCKED;
    for (unsigned index = 0; index < CGROUP_HIERARCHIES; index++) {
        struct cgroup *cgroup = parent ? parent->cgroups[index] : NULL;
        child->cgroups[index] = cgroup;
        if (cgroup) {
            cgroup->tasks++;
            cgroup->refs++;
        }
    }
}

static unsigned leave(struct process *process, unsigned index, struct cgroup **changed,
                      unsigned room) {
    struct cgroup *cgroup = process->cgroups[index];
    if (!cgroup) return 0;
    int was[NOTIFY_DEPTH];
    remember_populated(cgroup, was, NOTIFY_DEPTH);
    process->cgroups[index] = NULL;
    cgroup->tasks--;
    unsigned count = collect_populated(cgroup, was, changed, room);
    cgroup->refs--;
    return count;
}

void cgroup_exit(struct process *process) {
    if (!process) return;
    struct cgroup *changed[CGROUP_HIERARCHIES * NOTIFY_DEPTH];
    unsigned count = 0;
    {
        CGROUP_LOCKED;
        for (unsigned index = 0; index < CGROUP_HIERARCHIES; index++)
            count += leave(process, index, changed + count, NOTIFY_DEPTH);
    }
    notify_events(changed, count);
}

void cgroup_drop(struct process *process) {
    if (!process) return;
    struct cgroup *changed[NOTIFY_DEPTH];
    CGROUP_LOCKED;
    for (unsigned index = 0; index < CGROUP_HIERARCHIES; index++) {
        unsigned count = leave(process, index, changed, NOTIFY_DEPTH);
        for (unsigned item = 0; item < count; item++) changed[item]->refs--;
    }
}

static int64_t cgroup_path(const struct cgroup *cgroup, char *out, size_t capacity) {
    const struct hierarchy *hierarchy = cgroup->hierarchy;
    if (!hierarchy || !hierarchy->mount_root || !cgroup->dir || cgroup == hierarchy->root) {
        if (capacity < 2) return -1;
        out[0] = '/';
        out[1] = '\0';
        return 1;
    }
    char *full = vfs_path_buffer();
    char *base = vfs_path_buffer();
    int64_t length = -1;
    if (full && base && vfs_node_path(cgroup->dir, full, VFS_PATH_MAX) == 0 &&
        vfs_node_path(hierarchy->mount_root, base, VFS_PATH_MAX) == 0) {
        size_t skip = strlen(base);
        if (skip == 1) skip = 0;
        size_t rest = strlen(full + skip);
        if (rest + 1 <= capacity) {
            memcpy(out, full + skip, rest + 1);
            length = (int64_t)rest;
        }
    }
    vfs_path_release(&full);
    vfs_path_release(&base);
    return length;
}

size_t cgroup_describe(struct process *process, char *out, size_t capacity) {
    struct cgroup *members[CGROUP_HIERARCHIES];
    int used[CGROUP_HIERARCHIES];
    {
        CGROUP_LOCKED;
        for (unsigned index = 0; index < CGROUP_HIERARCHIES; index++) {
            used[index] = hierarchies[index].used;
            members[index] = used[index] ? member_of(process, index) : NULL;
            if (members[index]) members[index]->refs++;
        }
    }
    struct text text = {0};
    char path[VFS_PATH_MAX];
    for (unsigned pass = 0; pass < CGROUP_HIERARCHIES; pass++) {
        unsigned index = (pass + 1U) % CGROUP_HIERARCHIES;
        if (!members[index]) continue;
        if (index) {
            text_number(&text, index);
            text_string(&text, ":name=");
            text_string(&text, hierarchies[index].name);
            text_string(&text, ":");
        } else {
            text_string(&text, "0::");
        }
        if (cgroup_path(members[index], path, sizeof(path)) < 0) text_string(&text, "/");
        else text_string(&text, path);
        text_string(&text, "\n");
        cgroup_put(members[index]);
    }
    size_t length = text.length < capacity ? text.length : capacity;
    if (length) memcpy(out, text.data, length);
    text_free(&text);
    return length;
}

struct listing {
    struct cgroup *cgroup;
    unsigned index;
    int threads;
    uint64_t *ids;
    size_t count;
    size_t capacity;
};

static int list_member(struct process *process, void *context) {
    struct listing *listing = (struct listing *)context;
    CGROUP_LOCKED;
    if (!process_is_live(process)) return 0;
    if (!listing->threads && process->is_thread) return 0;
    if (member_of(process, listing->index) != listing->cgroup) return 0;
    if (listing->count == listing->capacity) return 1;
    listing->ids[listing->count++] = listing->threads ? process->pid : process->tgid;
    return 0;
}

static int64_t read_members(struct cgroup *cgroup, int threads, uint64_t offset, size_t size,
                            void *output) {
    struct listing listing = {cgroup, hierarchy_index(cgroup->hierarchy), threads, NULL, 0, 0};
    for (;;) {
        listing.capacity = listing.capacity ? listing.capacity * 2 : 256;
        listing.ids = (uint64_t *)kmalloc(listing.capacity * sizeof(uint64_t));
        if (!listing.ids) return -ENOMEM;
        listing.count = 0;
        process_for_each(list_member, &listing);
        if (listing.count < listing.capacity) break;
        kfree(listing.ids);
    }
    struct text text = {0};
    for (size_t index = 0; index < listing.count; index++) {
        text_number(&text, listing.ids[index]);
        text_string(&text, "\n");
    }
    kfree(listing.ids);
    return text_reply(&text, offset, size, output);
}

struct move {
    struct cgroup *to;
    unsigned index;
    uint64_t id;
    int whole_group;
    uint32_t caller_uid;
    int found;
    int denied;
    struct cgroup *changed[NOTIFY_DEPTH * 2];
    unsigned changed_count;
};

static int move_member(struct process *process, void *context) {
    struct move *move = (struct move *)context;
    CGROUP_LOCKED;
    if (!process_is_live(process)) return 0;
    if (move->whole_group ? process->tgid != move->id : process->pid != move->id) return 0;
    if (move->caller_uid && move->caller_uid != process->cred.euid &&
        move->caller_uid != process->cred.uid) {
        move->denied = 1;
        return 1;
    }
    move->found = 1;
    struct cgroup *from = member_of(process, move->index);
    if (from == move->to) return 0;
    if (move->changed_count < NOTIFY_DEPTH)
        move->changed_count += leave(process, move->index, move->changed + move->changed_count,
                                     NOTIFY_DEPTH - move->changed_count);
    else
        (void)leave(process, move->index, move->changed, 0);
    if (move->to != move->to->hierarchy->root) {
        int was[NOTIFY_DEPTH];
        remember_populated(move->to, was, NOTIFY_DEPTH);
        process->cgroups[move->index] = move->to;
        move->to->tasks++;
        move->to->refs++;
        unsigned room = NOTIFY_DEPTH * 2 - move->changed_count;
        move->changed_count += collect_populated(move->to, was,
                                                 move->changed + move->changed_count, room);
    }
    return 0;
}

static uint64_t parse_number(const char *text, size_t size, int *valid) {
    uint64_t value = 0;
    size_t index = 0;
    while (index < size && (text[index] == ' ' || text[index] == '\t')) index++;
    size_t start = index;
    while (index < size && text[index] >= '0' && text[index] <= '9') {
        value = value * 10U + (uint64_t)(text[index] - '0');
        index++;
    }
    *valid = index > start;
    while (index < size && (text[index] == '\n' || text[index] == ' ' || text[index] == '\0'))
        index++;
    if (index != size) *valid = 0;
    return value;
}

static int64_t write_members(struct cgroup *cgroup, int whole_group, size_t size,
                             const void *buffer) {
    int valid;
    uint64_t id = parse_number((const char *)buffer, size, &valid);
    if (!valid) return -EINVAL;
    struct process *caller = process_current();
    if (!id && caller) id = whole_group ? caller->tgid : caller->pid;
    struct move move;
    memset(&move, 0, sizeof(move));
    move.to = cgroup;
    move.index = hierarchy_index(cgroup->hierarchy);
    move.id = id;
    move.whole_group = whole_group;
    move.caller_uid = caller ? caller->cred.euid : 0;
    process_for_each(move_member, &move);
    notify_events(move.changed, move.changed_count);
    if (move.denied) return -EPERM;
    if (!move.found) return -ESRCH;
    return (int64_t)size;
}

struct kill {
    struct cgroup *cgroup;
    unsigned index;
    uint64_t *ids;
    size_t count;
    size_t capacity;
};

static int collect_victim(struct process *process, void *context) {
    struct kill *kill = (struct kill *)context;
    CGROUP_LOCKED;
    if (!process_is_live(process) || process->is_thread) return 0;
    if (!descends_from(member_of(process, kill->index), kill->cgroup)) return 0;
    if (kill->count < kill->capacity) kill->ids[kill->count++] = process->tgid;
    return 0;
}

static int64_t kill_members(struct cgroup *cgroup, size_t size) {
    struct kill kill = {cgroup, hierarchy_index(cgroup->hierarchy), NULL, 0, 4096};
    kill.ids = (uint64_t *)kmalloc(kill.capacity * sizeof(uint64_t));
    if (!kill.ids) return -ENOMEM;
    process_for_each(collect_victim, &kill);
    for (size_t index = 0; index < kill.count; index++)
        (void)process_send_signal((int64_t)kill.ids[index], SIGKILL);
    kfree(kill.ids);
    return (int64_t)size;
}

static int64_t control_read(struct vfs_node *node, uint64_t offset, size_t size, void *output) {
    struct cgroup_file *file = (struct cgroup_file *)node->fs_private;
    if (!file || !output) return 0;
    struct cgroup *cgroup = file->cgroup;
    struct text text = {0};
    switch (file->kind) {
    case FILE_PROCS: return read_members(cgroup, 0, offset, size, output);
    case FILE_THREADS:
    case FILE_TASKS: return read_members(cgroup, 1, offset, size, output);
    case FILE_CONTROLLERS:
    case FILE_SUBTREE: text_string(&text, "\n"); break;
    case FILE_TYPE: text_string(&text, "domain\n"); break;
    case FILE_EVENTS: {
        int populated, frozen;
        {
            CGROUP_LOCKED;
            populated = subtree_populated(cgroup);
            frozen = cgroup->frozen;
        }
        text_string(&text, populated ? "populated 1\n" : "populated 0\n");
        text_string(&text, frozen ? "frozen 1\n" : "frozen 0\n");
        break;
    }
    case FILE_STAT: {
        uint32_t descendants;
        {
            CGROUP_LOCKED;
            descendants = descendant_count(cgroup);
        }
        text_string(&text, "nr_descendants ");
        text_number(&text, descendants);
        text_string(&text, "\nnr_dying_descendants 0\n");
        break;
    }
    case FILE_FREEZE: text_string(&text, cgroup->frozen ? "1\n" : "0\n"); break;
    case FILE_MAX_DEPTH:
    case FILE_MAX_DESCENDANTS: text_string(&text, "max\n"); break;
    case FILE_NOTIFY: text_string(&text, cgroup->notify_on_release ? "1\n" : "0\n"); break;
    case FILE_CLONE_CHILDREN: text_string(&text, cgroup->clone_children ? "1\n" : "0\n"); break;
    case FILE_RELEASE_AGENT:
        text_string(&text, cgroup->hierarchy->release_agent);
        text_string(&text, "\n");
        break;
    case FILE_SANE: text_string(&text, "0\n"); break;
    default: break;
    }
    return text_reply(&text, offset, size, output);
}

static int written_flag(const void *buffer, size_t size, int *value) {
    int valid;
    uint64_t number = parse_number((const char *)buffer, size, &valid);
    if (!valid || number > 1) return -EINVAL;
    *value = (int)number;
    return 0;
}

static int64_t control_write(struct vfs_node *node, uint64_t offset, size_t size,
                          const void *buffer) {
    (void)offset;
    struct cgroup_file *file = (struct cgroup_file *)node->fs_private;
    if (!file || !buffer) return -EINVAL;
    struct cgroup *cgroup = file->cgroup;
    const char *text = (const char *)buffer;
    int flag;
    switch (file->kind) {
    case FILE_PROCS: return write_members(cgroup, 1, size, buffer);
    case FILE_THREADS:
    case FILE_TASKS: return write_members(cgroup, 0, size, buffer);
    case FILE_SUBTREE:
        for (size_t index = 0; index < size; index++)
            if (text[index] == '+') return -ENOENT;
        return (int64_t)size;
    case FILE_TYPE:
        return size >= 6 && memcmp(text, "domain", 6) == 0 ? (int64_t)size : -EINVAL;
    case FILE_FREEZE:
        if (written_flag(buffer, size, &flag) != 0) return -EINVAL;
        cgroup->frozen = (uint8_t)flag;
        if (cgroup->events) inotify_notify(cgroup->events, TUNIX_IN_MODIFY, NULL, 0);
        return (int64_t)size;
    case FILE_KILL:
        if (written_flag(buffer, size, &flag) != 0 || flag != 1) return -EINVAL;
        return kill_members(cgroup, size);
    case FILE_MAX_DEPTH:
    case FILE_MAX_DESCENDANTS: return (int64_t)size;
    case FILE_NOTIFY:
        if (written_flag(buffer, size, &flag) != 0) return -EINVAL;
        cgroup->notify_on_release = (uint8_t)flag;
        return (int64_t)size;
    case FILE_CLONE_CHILDREN:
        if (written_flag(buffer, size, &flag) != 0) return -EINVAL;
        cgroup->clone_children = (uint8_t)flag;
        return (int64_t)size;
    case FILE_RELEASE_AGENT: {
        size_t length = size;
        while (length && (text[length - 1] == '\n' || text[length - 1] == '\0')) length--;
        if (length >= sizeof(cgroup->hierarchy->release_agent)) return -EINVAL;
        memcpy(cgroup->hierarchy->release_agent, text, length);
        cgroup->hierarchy->release_agent[length] = '\0';
        return (int64_t)size;
    }
    default: return -EINVAL;
    }
}

static const char *const file_names[FILE_COUNT] = {
    "cgroup.procs", "cgroup.threads", "tasks", "cgroup.controllers",
    "cgroup.subtree_control", "cgroup.type", "cgroup.events", "cgroup.stat",
    "cgroup.freeze", "cgroup.kill", "cgroup.max.depth", "cgroup.max.descendants",
    "notify_on_release", "cgroup.clone_children", "release_agent", "cgroup.sane_behavior",
};

static int file_present(const struct cgroup *cgroup, unsigned kind) {
    int root = cgroup == cgroup->hierarchy->root;
    if (cgroup->hierarchy->v2) {
        switch (kind) {
        case FILE_PROCS: case FILE_THREADS: case FILE_CONTROLLERS: case FILE_SUBTREE:
        case FILE_STAT: case FILE_MAX_DEPTH: case FILE_MAX_DESCENDANTS:
            return 1;
        case FILE_TYPE: case FILE_EVENTS: case FILE_FREEZE: case FILE_KILL:
            return !root;
        default:
            return 0;
        }
    }
    switch (kind) {
    case FILE_PROCS: case FILE_TASKS: case FILE_NOTIFY: case FILE_CLONE_CHILDREN:
        return 1;
    case FILE_RELEASE_AGENT: case FILE_SANE:
        return root;
    default:
        return 0;
    }
}

static uint32_t file_mode(unsigned kind) {
    switch (kind) {
    case FILE_CONTROLLERS: case FILE_EVENTS: case FILE_STAT: case FILE_SANE:
        return 0444;
    case FILE_KILL:
        return 0200;
    default:
        return 0644;
    }
}

static int adopt(struct vfs_node *directory, struct vfs_node *child);

static void populate(struct cgroup *cgroup) {
    struct vfs_node *directory = cgroup->dir;
    directory->fs_private = cgroup;
    directory->adopt = adopt;
    for (unsigned kind = 0; kind < FILE_COUNT; kind++) {
        cgroup->files[kind].cgroup = cgroup;
        cgroup->files[kind].kind = kind;
        if (!file_present(cgroup, kind)) continue;
        struct vfs_node *node = vfs_alloc_node(file_names[kind], VFS_FILE);
        if (!node) continue;
        node->mode = file_mode(kind);
        node->uid = directory->uid;
        node->gid = directory->gid;
        node->read = control_read;
        node->write = control_write;
        node->fs_private = &cgroup->files[kind];
        if (vfs_attach(directory, node) != 0) {
            vfs_free_node(node);
            continue;
        }
        if (kind == FILE_EVENTS) cgroup->events = node;
    }
}

static int is_cgroup_directory(const struct vfs_node *node) {
    return node && (node->flags & 0xFFU) == VFS_DIRECTORY && node->adopt == adopt &&
           node->fs_private;
}

static int adopt(struct vfs_node *directory, struct vfs_node *child) {
    if (!is_cgroup_directory(directory) || (child->flags & 0xFFU) != VFS_DIRECTORY) return 0;
    struct cgroup *parent = (struct cgroup *)directory->fs_private;
    struct cgroup *cgroup = (struct cgroup *)kmalloc(sizeof(*cgroup));
    if (!cgroup) return -1;
    memset(cgroup, 0, sizeof(*cgroup));
    cgroup->hierarchy = parent->hierarchy;
    cgroup->dir = child;
    cgroup->refs = 1;
    cgroup->notify_on_release = parent->notify_on_release;
    child->mode = (child->mode & ~0777U) | 0755U;
    {
        CGROUP_LOCKED;
        cgroup->parent = parent;
        cgroup->sibling = parent->children;
        parent->children = cgroup;
        parent->refs++;
    }
    populate(cgroup);
    return 0;
}

static int rmdir_locked(struct vfs_node *directory);

int cgroupfs_is_cgroup(const struct vfs_node *node) {
    if (!is_cgroup_directory(node)) return 0;
    const struct cgroup *cgroup = (const struct cgroup *)node->fs_private;
    return cgroup != cgroup->hierarchy->root;
}

int cgroupfs_rmdir(struct vfs_node *directory) {
    vfs_lock_acquire();
    int status = rmdir_locked(directory);
    vfs_lock_release();
    return status;
}

static int rmdir_locked(struct vfs_node *directory) {
    if (!cgroupfs_is_cgroup(directory)) return -EINVAL;
    struct cgroup *cgroup = (struct cgroup *)directory->fs_private;
    for (struct vfs_node *child = directory->children; child; child = child->next)
        if ((child->flags & 0xFFU) == VFS_DIRECTORY) return -ENOTEMPTY;
    struct cgroup *parent;
    {
        CGROUP_LOCKED;
        if (cgroup->tasks || cgroup->children) return -EBUSY;
        parent = cgroup->parent;
        struct cgroup **link = &parent->children;
        while (*link && *link != cgroup) link = &(*link)->sibling;
        if (*link) *link = cgroup->sibling;
        cgroup->parent = NULL;
        cgroup->events = NULL;
        cgroup->dir = NULL;
    }
    while (directory->children) {
        struct vfs_node *child = directory->children;
        inotify_invalidate(child);
        (void)vfs_detach_child(directory, child);
    }
    directory->fs_private = NULL;
    directory->adopt = NULL;
    cgroup_put(cgroup);
    cgroup_put(parent);
    return 0;
}

static int parse_options(const char *options, char *name, size_t capacity) {
    name[0] = '\0';
    if (!options) return 0;
    const char *at = options;
    while (*at) {
        const char *end = at;
        while (*end && *end != ',') end++;
        size_t length = (size_t)(end - at);
        if (length > 5 && memcmp(at, "name=", 5) == 0) {
            if (length - 5 >= capacity) return -EINVAL;
            memcpy(name, at + 5, length - 5);
            name[length - 5] = '\0';
        } else if (!(length == 4 && memcmp(at, "none", 4) == 0) &&
                   !(length >= 14 && memcmp(at, "release_agent=", 14) == 0) &&
                   !(length == 5 && memcmp(at, "xattr", 5) == 0) &&
                   !(length == 2 && memcmp(at, "rw", 2) == 0) &&
                   !(length == 10 && memcmp(at, "nsdelegate", 10) == 0) &&
                   !(length == 20 && memcmp(at, "memory_recursiveprot", 20) == 0) &&
                   length) {
            return -ENOENT;
        }
        at = *end ? end + 1 : end;
    }
    return 0;
}

static void apply_release_agent(struct hierarchy *hierarchy, const char *options) {
    if (!options) return;
    const char *at = options;
    while (*at) {
        const char *end = at;
        while (*end && *end != ',') end++;
        size_t length = (size_t)(end - at);
        if (length > 14 && memcmp(at, "release_agent=", 14) == 0 &&
            length - 14 < sizeof(hierarchy->release_agent)) {
            memcpy(hierarchy->release_agent, at + 14, length - 14);
            hierarchy->release_agent[length - 14] = '\0';
        }
        at = *end ? end + 1 : end;
    }
}

int cgroupfs_mount(const char *type, const char *options, const char *name,
                   struct vfs_node **root) {
    int v2 = strcmp(type, "cgroup2") == 0;
    char wanted[32];
    if (!v2) {
        int status = parse_options(options, wanted, sizeof(wanted));
        if (status != 0) return status;
        if (!wanted[0]) return -ENOENT;
    }

    struct hierarchy *hierarchy = NULL;
    struct cgroup *fresh = NULL;
    if (v2) {
        hierarchy = &hierarchies[0];
    } else {
        for (unsigned index = 1; index < CGROUP_HIERARCHIES; index++)
            if (hierarchies[index].used && strcmp(hierarchies[index].name, wanted) == 0)
                hierarchy = &hierarchies[index];
        if (!hierarchy) {
            fresh = (struct cgroup *)kmalloc(sizeof(*fresh));
            if (!fresh) return -ENOMEM;
            memset(fresh, 0, sizeof(*fresh));
            fresh->refs = 1;
            for (unsigned index = 1; index < CGROUP_HIERARCHIES && !hierarchy; index++)
                if (!hierarchies[index].used) hierarchy = &hierarchies[index];
            if (!hierarchy) {
                kfree(fresh);
                return -ENOMEM;
            }
        }
    }
    if (hierarchy->mount_root) {
        kfree(fresh);
        return -EBUSY;
    }

    struct vfs_node *node = vfs_alloc_node(name, VFS_DIRECTORY | VFS_VOLATILE);
    if (!node) {
        kfree(fresh);
        return -ENOMEM;
    }
    node->mode = 0755;
    if (fresh) {
        CGROUP_LOCKED;
        hierarchy->root = fresh;
        strncpy(hierarchy->name, wanted, sizeof(hierarchy->name) - 1);
        hierarchy->used = 1;
    }
    if (!v2) apply_release_agent(hierarchy, options);
    hierarchy->root->hierarchy = hierarchy;
    hierarchy->root->dir = node;
    hierarchy->mount_root = node;
    populate(hierarchy->root);
    *root = node;
    return 0;
}

int cgroupfs_unmount(struct vfs_node *root) {
    for (unsigned index = 0; index < CGROUP_HIERARCHIES; index++) {
        struct hierarchy *hierarchy = &hierarchies[index];
        if (!hierarchy->used || hierarchy->mount_root != root) continue;
        CGROUP_LOCKED;
        if (hierarchy->root->children) return -EBUSY;
        hierarchy->root->dir = NULL;
        hierarchy->root->events = NULL;
        hierarchy->mount_root = NULL;
        root->fs_private = NULL;
        root->adopt = NULL;
        return 0;
    }
    return 0;
}

uint32_t cgroupfs_magic(const struct vfs_node *node) {
    for (unsigned index = 0; index < CGROUP_HIERARCHIES; index++)
        if (hierarchies[index].used && hierarchies[index].mount_root == node)
            return hierarchies[index].v2 ? CGROUP2_SUPER_MAGIC : CGROUP_SUPER_MAGIC;
    return 0;
}
