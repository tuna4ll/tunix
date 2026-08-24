#include <stddef.h>
#include <stdint.h>
#include "include/cred.h"
#include "include/heap.h"
#include "include/inotify.h"
#include "include/kstring.h"
#include "include/time.h"
#include "include/fatfs.h"
#include "include/vfs.h"

#define VFS_PATH_MAX 256
#define VFS_SYMLINK_MAX_DEPTH 16
#define VFS_MOUNT_MAX_DEPTH 16
#define VFS_TREE_MAX_DEPTH 64

struct vfs_node *vfs_root;
static uint64_t next_inode = 1;
static const struct vfs_persist_ops *persist_ops;

/*
 * Two dispatches, not one. persist_ops belongs to the filesystem that owns the
 * root and is a single global; a second filesystem mounted somewhere else
 * cannot take it away. So a node's own directory gets asked as well: a driver
 * that sets `adopt` on the directories it built is told about children created
 * inside them, which is how a file created on a FAT mount reaches the medium
 * rather than living in RAM until the mount goes away.
 */
#define PERSIST(op, ...) \
    do { if (persist_ops && persist_ops->op) persist_ops->op(__VA_ARGS__); } while (0)

static void adopt_into_parent(struct vfs_node *node) {
    if (node && node->parent && node->parent->adopt)
        (void)node->parent->adopt(node->parent, node);
}

void vfs_set_persist_ops(const struct vfs_persist_ops *ops) {
    persist_ops = ops;
}

void vfs_stamp_times(struct vfs_node *node, uint32_t which) {
    if (!node) return;
    uint32_t now = (uint32_t)time_epoch_seconds();
    if (which & VFS_TIME_ATIME) node->atime = now;
    if (which & VFS_TIME_MTIME) node->mtime = now;
    if (which & VFS_TIME_CTIME) node->ctime = now;
}

void vfs_notify_meta_changed(struct vfs_node *node) {
    vfs_stamp_times(node, VFS_TIME_CTIME);
    PERSIST(meta_changed, node);
}

/*
 * Bytes of file content sitting in the heap that the disk could hand back.
 *
 * Kept as a running total rather than measured, because the budget below is
 * consulted on every syscall and walking the tree to answer would cost more
 * than the cache saves. Every transition into and out of the cacheable state
 * goes through cache_charge()/cache_discharge(), which is the whole of it.
 */
static uint64_t cached_bytes;

/* Whether this node's contents are the kind the disk can replace. */
static int cacheable(const struct vfs_node *node) {
    return node && (node->flags & 0xFFU) == VFS_FILE && node->disk_inode &&
           node->data && (node->flags & VFS_OWNED_DATA);
}

static void cache_charge(const struct vfs_node *node) {
    if (cacheable(node)) cached_bytes += node->capacity;
}

static void cache_discharge(const struct vfs_node *node) {
    if (!cacheable(node)) return;
    cached_bytes -= cached_bytes >= node->capacity ? node->capacity : cached_bytes;
}

uint64_t vfs_cached_bytes(void) { return cached_bytes; }

/* Mount restores only the tree's shape; the first read, write, exec or mmap
   of a file comes through here to pull its contents off the disk. */
int vfs_fault_in(struct vfs_node *node) {
    if (!node) return -1;
    if (!(node->flags & VFS_LAZY_DATA)) return 0;
    if (!persist_ops || !persist_ops->fetch) return -1;
    if (persist_ops->fetch(node) != 0) return -1;
    /* the fetch owes us length bytes; everything below dereferences them */
    if (!node->data && node->length) return -1;
    node->flags &= ~VFS_LAZY_DATA;
    cache_charge(node);
    return 0;
}

void vfs_map_ref(struct vfs_node *node) {
    if (node) node->mapped_refs++;
}

void vfs_map_unref(struct vfs_node *node) {
    if (node && node->mapped_refs) node->mapped_refs--;
}

/* mmap copies the whole file into the process; keeping the kernel's copy as
   well doubles the cost of every shared library on the image. */
void vfs_release_data(struct vfs_node *node) {
    if (!cacheable(node)) return;
    if (node->mapped_refs) return;
    if (!persist_ops || !persist_ops->fetch) return;
    cache_discharge(node);
    kfree(node->data);
    node->data = NULL;
    node->capacity = 0;
    node->flags = (node->flags & ~VFS_OWNED_DATA) | VFS_LAZY_DATA;
}

/* Drop every file body that the disk can hand back, and say how many bytes that
 * returned to the heap.
 *
 * Tunix keeps file contents in kmalloc'd buffers, and the heap never gives
 * pages back to the PMM. So the first read or write of a file converts general
 * memory into heap memory permanently: browse for a few minutes, and the
 * browser's cache alone can push the heap to its ceiling, after which kmalloc
 * returns NULL and nothing new can start -- the machine stays up and refuses to
 * launch anything, which is a confusing way to run out of memory.
 *
 * Reclaiming is only dropping a cache. Writes are persisted as they happen
 * (PERSIST(written, ...)), so the disk copy is authoritative and vfs_fault_in()
 * pulls the bytes back on the next access. vfs_release_data() already declines
 * the nodes where that is not true: anything with no disk inode behind it, and
 * anything mapped into a process.
 */
static uint64_t reclaim_below(struct vfs_node *node, uint32_t newer_than) {
    if (!node || node->link_target) return 0;

    uint64_t reclaimed = 0;
    /*
     * A file touched a moment ago is one something is working through;
     * dropping it only to read it straight back is worse than keeping it.
     *
     * "Touched" has to mean written as well as read. Only atime was consulted
     * here, and a write does not set atime -- so the file a process was in the
     * middle of writing looked like the coldest thing in the tree and was
     * always the first to go. The next write then faulted the whole file back
     * off the disk before it could add a byte, and again for the byte after
     * that: downloading a 78 MB package started at 730 KB/s and was down to
     * 110 KB/s by the time it was two thirds through, with the processor
     * inside the ATA driver the whole way.
     */
    uint32_t touched = node->atime > node->mtime ? node->atime : node->mtime;
    if ((node->flags & 0xFFU) == VFS_FILE && touched < newer_than) {
        uint64_t held = node->capacity;
        vfs_release_data(node);
        if (!node->data) reclaimed += held;
    }

    for (struct vfs_node *child = node->children; child; child = child->next)
        reclaimed += reclaim_below(child, newer_than);

    return reclaimed;
}

uint64_t vfs_reclaim_file_data(struct vfs_node *node) {
    return reclaim_below(node, 0xFFFFFFFFU);
}

/*
 * Cut every file loose from contents the VFS does not own.
 *
 * The initramfs tree is built by pointing each node straight at the archive in
 * physical memory instead of copying it, so the whole image has to stay
 * reserved for as long as any node refers to it. Once it has been seeded to
 * disk each of those files has an inode of its own and can be fetched back, so
 * the pointers can go -- and with them the reservation.
 *
 * A node someone has mapped keeps its pointer: the pages are in that process's
 * page tables and the archive underneath them may not move.
 */
uint64_t vfs_detach_static_data(struct vfs_node *node) {
    if (!node) return 0;

    uint64_t detached = 0;
    if ((node->flags & 0xFFU) == VFS_FILE && node->data && node->length &&
        node->disk_inode && !(node->flags & VFS_OWNED_DATA) && !node->mapped_refs) {
        node->data = NULL;
        node->capacity = 0;
        node->flags |= VFS_LAZY_DATA;
        detached++;
    }

    for (struct vfs_node *child = node->children; child; child = child->next)
        detached += vfs_detach_static_data(child);

    return detached;
}

/*
 * Hold the cache to its budget.
 *
 * Called at every syscall entry, so the common case has to be a comparison and
 * nothing more. When it does fire it starts by dropping only what has not been
 * touched recently, and widens the window until the cache fits or there is
 * nothing older left -- which is as close to least-recently-used as a tree with
 * one-second timestamps and no list can get.
 */
void vfs_trim_cache(uint64_t budget) {
    /* When a pass cannot get under the budget -- everything left is mapped, or
       was touched a moment ago -- retrying on the next syscall would walk the
       whole tree for nothing, over and over. Wait until the cache has grown
       appreciably again before spending another walk on it. */
    static uint64_t retry_above;

    if (!budget || cached_bytes <= budget || cached_bytes < retry_above) return;

    uint32_t now = (uint32_t)time_epoch_seconds();
    static const uint32_t ages[] = { 60U, 10U, 1U, 0U };
    for (unsigned index = 0; index < sizeof(ages) / sizeof(ages[0]); index++) {
        uint32_t cutoff = now > ages[index] ? now - ages[index] : 0;
        (void)reclaim_below(vfs_root, cutoff);
        if (cached_bytes <= budget) {
            retry_above = 0;
            return;
        }
    }
    retry_above = cached_bytes + budget / 8;
}

/* Mapped data is being read through some process's page tables; releasing it
   would pull the pages out from under that mapping, so it is left alone. The
   node is going away either way, which is why this is the one place the count
   can be non-zero and the memory still has to be given up on. */
static void free_node_data(struct vfs_node *node) {
    if (!node || !node->data || !(node->flags & VFS_OWNED_DATA)) return;
    if (node->mapped_refs) return;
    cache_discharge(node);
    kfree(node->data);
}

static int valid_component(const char *name) {
    return name && name[0] && strcmp(name, ".") != 0 && strcmp(name, "..") != 0;
}

struct vfs_node *vfs_alloc_node(const char *name, uint32_t flags) {
    struct vfs_node *node = (struct vfs_node *)kmalloc(sizeof(*node));
    if (!node) return NULL;
    memset(node, 0, sizeof(*node));
    strncpy(node->name, name ? name : "", sizeof(node->name) - 1);
    node->flags = flags;
    node->inode = next_inode++;
    node->links = 1;
    uint32_t kind = flags & 0xFFU;
    node->mode = kind == VFS_DIRECTORY ? 0755 : (kind == VFS_SYMLINK ? 0777 : 0644);
    vfs_stamp_times(node, VFS_TIME_ATIME | VFS_TIME_MTIME | VFS_TIME_CTIME);
    return node;
}

void vfs_init(void) {
    vfs_root = vfs_alloc_node("/", VFS_DIRECTORY);
    if (vfs_root) vfs_root->parent = vfs_root;
}

int vfs_attach(struct vfs_node *parent, struct vfs_node *child) {
    if (!parent || !child || (parent->flags & 0xFFU) != VFS_DIRECTORY) return -1;
    if (vfs_find_child(parent, child->name)) return -2;
    child->parent = parent;
    child->next = NULL;
    if (!parent->children) parent->children = child;
    else {
        struct vfs_node *tail = parent->children;
        while (tail->next) tail = tail->next;
        tail->next = child;
    }
    return 0;
}

struct vfs_node *vfs_find_entry(struct vfs_node *directory, const char *name) {
    if (!directory || !name || (directory->flags & 0xFFU) != VFS_DIRECTORY) return NULL;
    if (directory->refresh) directory->refresh(directory);
    for (struct vfs_node *node = directory->children; node; node = node->next) {
        if (strcmp(node->name, name) == 0) return node;
    }
    return NULL;
}

/* Step onto whatever is mounted over a directory. A loop rather than one hop
   because a mount can be made over a directory that is itself a mount root. */
static struct vfs_node *cross_mounts(struct vfs_node *node) {
    unsigned depth = 0;
    while (node && node->mounted && depth++ < VFS_MOUNT_MAX_DEPTH)
        node = node->mounted;
    return node;
}

/* Resolving here is what keeps hard links and mounts out of the rest of the
   kernel: every path walk goes through this, so a second name reaches the same
   node as the first, a mounted directory reaches the mounted tree, and nothing
   downstream has to know which it was reached by. */
struct vfs_node *vfs_find_child(struct vfs_node *directory, const char *name) {
    struct vfs_node *node = vfs_find_entry(directory, name);
    if (node && node->link_target) node = node->link_target;
    return cross_mounts(node);
}

static const char *next_component(const char *path, char component[128]) {
    while (*path == '/') path++;
    size_t length = 0;
    while (*path && *path != '/') {
        if (length + 1 < 128) component[length++] = *path;
        path++;
    }
    component[length] = '\0';
    while (*path == '/') path++;
    return path;
}

int vfs_node_path(struct vfs_node *node, char *buffer, size_t capacity) {
    if (!node || !buffer || capacity < 2) return -1;
    if (node == vfs_root) {
        buffer[0] = '/'; buffer[1] = '\0'; return 0;
    }
    const struct vfs_node *stack[64];
    size_t depth = 0;
    while (node && node != vfs_root && depth < 64) {
        stack[depth++] = node;
        node = node->parent;
    }
    if (node != vfs_root) return -1;
    size_t at = 0;
    buffer[at++] = '/';
    while (depth) {
        const char *name = stack[--depth]->name;
        size_t length = strlen(name);
        if (at + length + (depth ? 1 : 0) + 1 > capacity) return -1;
        memcpy(buffer + at, name, length); at += length;
        if (depth) buffer[at++] = '/';
    }
    buffer[at] = '\0';
    return 0;
}

static int append_text(char *output, size_t capacity, size_t *at, const char *text) {
    size_t length = strlen(text);
    if (*at + length + 1 > capacity) return -1;
    memcpy(output + *at, text, length);
    *at += length;
    output[*at] = '\0';
    return 0;
}

static struct vfs_node *lookup_internal(const char *path, int follow_final, unsigned depth) {
    if (!path || path[0] != '/' || !vfs_root || depth > VFS_SYMLINK_MAX_DEPTH) return NULL;
    if (path[1] == '\0') return cross_mounts(vfs_root);

    struct vfs_node *current = cross_mounts(vfs_root);
    char component[128];
    const char *cursor = path;
    while (*cursor) {
        cursor = next_component(cursor, component);
        if (!component[0] || strcmp(component, ".") == 0) continue;
        if (strcmp(component, "..") == 0) {
            current = current->parent ? current->parent : current;
            continue;
        }

        struct vfs_node *next = vfs_find_child(current, component);
        if (!next) return NULL;
        int final_component = *cursor == '\0';
        if ((next->flags & 0xFFU) == VFS_SYMLINK && (follow_final || !final_component)) {
            const char *target = (const char *)next->data;
            if (!target || !target[0]) return NULL;
            char resolved[VFS_PATH_MAX];
            size_t at = 0;
            resolved[0] = '\0';
            if (target[0] == '/') {
                if (append_text(resolved, sizeof(resolved), &at, target) != 0) return NULL;
            } else {
                char parent_path[VFS_PATH_MAX];
                if (vfs_node_path(current, parent_path, sizeof(parent_path)) != 0) return NULL;
                if (append_text(resolved, sizeof(resolved), &at, parent_path) != 0) return NULL;
                if (at > 1 && resolved[at - 1] != '/') {
                    if (append_text(resolved, sizeof(resolved), &at, "/") != 0) return NULL;
                }
                if (append_text(resolved, sizeof(resolved), &at, target) != 0) return NULL;
            }
            if (*cursor) {
                if (at == 0 || resolved[at - 1] != '/') {
                    if (append_text(resolved, sizeof(resolved), &at, "/") != 0) return NULL;
                }
                if (append_text(resolved, sizeof(resolved), &at, cursor) != 0) return NULL;
            }
            return lookup_internal(resolved, follow_final, depth + 1);
        }
        current = next;
    }
    return current;
}

struct vfs_node *vfs_lookup(const char *path) {
    return lookup_internal(path, 1, 0);
}

struct vfs_node *vfs_lookup_nofollow(const char *path) {
    return lookup_internal(path, 0, 0);
}

struct vfs_node *vfs_mkdir_p(const char *path) {
    if (!path || path[0] != '/' || !vfs_root) return NULL;
    struct vfs_node *current = vfs_root;
    char component[128];
    const char *cursor = path;
    while (*cursor) {
        cursor = next_component(cursor, component);
        if (!component[0]) break;
        if (!valid_component(component)) continue;
        struct vfs_node *next = vfs_find_child(current, component);
        if (!next) {
            next = vfs_alloc_node(component, VFS_DIRECTORY);
            if (!next || vfs_attach(current, next) != 0) return NULL;
            PERSIST(created, next);
            adopt_into_parent(next);
        } else if ((next->flags & 0xFFU) == VFS_SYMLINK) {
            char next_path[VFS_PATH_MAX];
            if (vfs_node_path(next, next_path, sizeof(next_path)) != 0) return NULL;
            next = vfs_lookup(next_path);
            if (!next) return NULL;
        }
        if ((next->flags & 0xFFU) != VFS_DIRECTORY) return NULL;
        current = next;
    }
    return current;
}

static int split_parent(const char *path, char parent[256], char name[128]) {
    if (!path || path[0] != '/') return -1;
    size_t length = strlen(path);
    while (length > 1 && path[length - 1] == '/') length--;
    size_t slash = length;
    while (slash > 0 && path[slash - 1] != '/') slash--;
    size_t name_length = length - slash;
    if (!name_length || name_length >= 128) return -1;
    memcpy(name, path + slash, name_length);
    name[name_length] = '\0';
    if (!valid_component(name)) return -1;
    if (slash <= 1) {
        parent[0] = '/';
        parent[1] = '\0';
    } else {
        size_t parent_length = slash - 1;
        if (parent_length >= 256) return -1;
        memcpy(parent, path, parent_length);
        parent[parent_length] = '\0';
    }
    return 0;
}

static int64_t memory_read(struct vfs_node *node, uint64_t offset, size_t size, void *buffer) {
    /* length first: a read at EOF must not fault the file in. */
    if (!node || !buffer || offset >= node->length) return 0;
    if (vfs_fault_in(node) != 0) return -1;
    uint64_t available = node->length - offset;
    if ((uint64_t)size > available) size = (size_t)available;
    memcpy(buffer, (const uint8_t *)node->data + offset, size);
    /*
     * A read is a use, and nothing else here said so: atime was never stamped
     * on the read path, so the reclaimer -- which decides what to drop by how
     * long ago a file was touched -- could not see that a file was being read
     * at all. It would drop the very file a program was working through, and
     * the next read pulled the whole thing back off the disk. Extracting a
     * 78 MB package is a stream of reads over one such file.
     *
     * Only the in-memory stamp; nothing is written to the disk for it, which
     * is what makes this affordable on every read.
     */
    if (size) vfs_stamp_times(node, VFS_TIME_ATIME);
    return (int64_t)size;
}

/*
 * Put a file's cached contents on a page boundary.
 *
 * mmap can only hand the cached pages themselves to a process when they start
 * on one, and the heap only aligns allocations of 64 KiB and up. Without this,
 * a shared mapping of a large file worked and a shared mapping of a small one
 * quietly fell back to private copies -- so whether writes reached the file
 * depended on its size, which is the worst of both answers. Reallocating at
 * the heap's alignment threshold costs padding on a small file, and only for
 * files somebody actually maps.
 */
#define VFS_PAGE_ALIGN_MIN (64ULL * 1024ULL)

int vfs_align_data(struct vfs_node *node) {
    if (!node || !node->data) return -1;
    if ((((uint64_t)node->data) & 0xFFFULL) == 0) return 0;
    if (!(node->flags & VFS_OWNED_DATA)) return -1;

    uint64_t capacity = node->capacity > VFS_PAGE_ALIGN_MIN ? node->capacity
                                                            : VFS_PAGE_ALIGN_MIN;
    uint8_t *aligned = (uint8_t *)kmalloc((size_t)capacity);
    if (!aligned) return -1;
    if ((((uint64_t)aligned) & 0xFFFULL) != 0) {
        kfree(aligned);
        return -1;
    }
    memset(aligned, 0, (size_t)capacity);
    if (node->length) memcpy(aligned, node->data, (size_t)node->length);
    free_node_data(node);
    node->data = aligned;
    node->capacity = capacity;
    node->flags |= VFS_OWNED_DATA;
    cache_charge(node);
    return 0;
}

static int ensure_capacity(struct vfs_node *node, uint64_t required) {
    if (required <= node->capacity) return 0;
    if (node->flags & VFS_READONLY) return -1;
    uint64_t capacity = node->capacity ? node->capacity : 64;
    while (capacity < required) {
        if (capacity > UINT64_MAX / 2) return -1;
        capacity *= 2;
    }
    uint8_t *new_data = (uint8_t *)kmalloc((size_t)capacity);
    if (!new_data) return -1;
    memset(new_data, 0, (size_t)capacity);
    if (node->data && node->length) memcpy(new_data, node->data, (size_t)node->length);
    free_node_data(node);
    node->data = new_data;
    node->capacity = capacity;
    node->flags |= VFS_OWNED_DATA;
    cache_charge(node);
    return 0;
}

static int64_t memory_write(struct vfs_node *node, uint64_t offset, size_t size, const void *buffer) {
    if (!node || !buffer || (node->flags & VFS_READONLY)) return -1;
    if ((uint64_t)size > UINT64_MAX - offset) return -1;
    /* a partial write still has to keep the bytes it does not cover */
    if (vfs_fault_in(node) != 0) return -1;
    uint64_t end = offset + size;
    if (ensure_capacity(node, end) != 0) return -1;
    memcpy((uint8_t *)node->data + offset, buffer, size);
    if (end > node->length) node->length = end;
    if (size) {
        vfs_stamp_times(node, VFS_TIME_MTIME | VFS_TIME_CTIME);
        inotify_notify(node, TUNIX_IN_MODIFY, NULL, 0);
        PERSIST(written, node, offset, size);
    }
    return (int64_t)size;
}

void vfs_setup_memory_file(struct vfs_node *node) {
    if (!node) return;
    node->read = memory_read;
    if (!(node->flags & VFS_READONLY)) node->write = memory_write;
}

struct vfs_node *vfs_create_file(const char *path, const void *data,
                                 uint64_t length, uint32_t flags, int copy_data) {
    char parent_path[256];
    char name[128];
    if (split_parent(path, parent_path, name) != 0) return NULL;
    struct vfs_node *parent = vfs_mkdir_p(parent_path);
    if (!parent || vfs_find_child(parent, name)) return NULL;

    struct vfs_node *node = vfs_alloc_node(name, VFS_FILE | flags);
    if (!node) return NULL;
    node->length = length;
    node->capacity = length;
    if (length && copy_data) {
        node->data = kmalloc((size_t)length);
        if (!node->data) { kfree(node); return NULL; }
        memcpy(node->data, data, (size_t)length);
        node->flags |= VFS_OWNED_DATA;
    } else node->data = (void *)data;
    node->read = memory_read;
    if (!(flags & VFS_READONLY)) node->write = memory_write;
    if (vfs_attach(parent, node) != 0) {
        free_node_data(node);
        kfree(node);
        return NULL;
    }
    inotify_notify(parent, TUNIX_IN_CREATE, name, 0);
    PERSIST(created, node);
    adopt_into_parent(node);
    return node;
}

struct vfs_node *vfs_create_file_node(const char *path, uint32_t mode) {
    char parent_path[256];
    char name[128];
    if (split_parent(path, parent_path, name) != 0) return NULL;
    struct vfs_node *parent = vfs_lookup(parent_path);
    if (!parent || (parent->flags & 0xFFU) != VFS_DIRECTORY) return NULL;
    if (vfs_find_child(parent, name)) return NULL;

    struct vfs_node *node = vfs_alloc_node(name, VFS_FILE);
    if (!node) return NULL;
    node->mode = mode & 07777U;
    node->read = memory_read;
    node->write = memory_write;
    if (vfs_attach(parent, node) != 0) {
        kfree(node);
        return NULL;
    }
    cred_stamp_new_node(node);
    inotify_notify(parent, TUNIX_IN_CREATE, name, 0);
    PERSIST(created, node);
    adopt_into_parent(node);
    return node;
}

struct vfs_node *vfs_create_directory(const char *path, uint32_t mode) {
    char parent_path[256];
    char name[128];
    if (split_parent(path, parent_path, name) != 0) return NULL;
    struct vfs_node *parent = vfs_lookup(parent_path);
    if (!parent || (parent->flags & 0xFFU) != VFS_DIRECTORY) return NULL;
    if (vfs_find_child(parent, name)) return NULL;

    struct vfs_node *node = vfs_alloc_node(name, VFS_DIRECTORY);
    if (!node) return NULL;
    node->mode = mode & 07777U;
    if (vfs_attach(parent, node) != 0) {
        kfree(node);
        return NULL;
    }
    cred_stamp_new_node(node);
    inotify_notify(parent, TUNIX_IN_CREATE, name, 0);
    PERSIST(created, node);
    adopt_into_parent(node);
    return node;
}

struct vfs_node *vfs_create_symlink(const char *path, const char *target,
                                    uint32_t flags) {
    if (!target || !target[0]) return NULL;
    char parent_path[256];
    char name[128];
    if (split_parent(path, parent_path, name) != 0) return NULL;
    struct vfs_node *parent = vfs_mkdir_p(parent_path);
    if (!parent || vfs_find_child(parent, name)) return NULL;

    struct vfs_node *node = vfs_alloc_node(name, VFS_SYMLINK | flags | VFS_OWNED_DATA);
    if (!node) return NULL;
    size_t length = strlen(target);
    node->data = kmalloc(length + 1);
    if (!node->data) { kfree(node); return NULL; }
    memcpy(node->data, target, length + 1);
    node->length = length;
    node->capacity = length + 1;
    if (vfs_attach(parent, node) != 0) {
        kfree(node->data);
        kfree(node);
        return NULL;
    }
    cred_stamp_new_node(node);
    inotify_notify(parent, TUNIX_IN_CREATE, name, 0);
    PERSIST(created, node);
    adopt_into_parent(node);
    return node;
}

struct vfs_node *vfs_attach_symlink(struct vfs_node *parent, const char *name,
                                    const char *target) {
    if (!parent || !name || !target || !target[0]) return NULL;
    struct vfs_node *node = vfs_alloc_node(name, VFS_SYMLINK | VFS_OWNED_DATA | VFS_VOLATILE);
    if (!node) return NULL;
    size_t length = strlen(target);
    node->data = kmalloc(length + 1);
    if (!node->data) { kfree(node); return NULL; }
    memcpy(node->data, target, length + 1);
    node->length = length;
    node->capacity = length + 1;
    if (vfs_attach(parent, node) != 0) {
        kfree(node->data);
        kfree(node);
        return NULL;
    }
    return node;
}

struct vfs_node *vfs_attach_link(struct vfs_node *parent, const char *name,
                                 struct vfs_node *target) {
    if (!parent || !name || !target) return NULL;
    if (target->link_target) target = target->link_target;
    uint32_t kind = target->flags & 0xFFU;
    if (kind == VFS_DIRECTORY) return NULL;

    /* The kind is copied so readdir can answer without following the link. */
    struct vfs_node *link = vfs_alloc_node(name, kind | VFS_HARDLINK);
    if (!link) return NULL;
    link->link_target = target;
    if (vfs_attach(parent, link) != 0) {
        kfree(link);
        return NULL;
    }
    target->links++;
    /* The reference is what lets the contents outlive their own name. */
    vfs_node_ref(target);
    return link;
}

int vfs_link(struct vfs_node *target, const char *path) {
    char parent_path[256];
    char name[128];
    if (!target || split_parent(path, parent_path, name) != 0) return -1;
    if (target->link_target) target = target->link_target;
    if ((target->flags & 0xFFU) == VFS_DIRECTORY) return -1;
    if (target->flags & VFS_READONLY) return -1;

    struct vfs_node *parent = vfs_lookup(parent_path);
    if (!parent || (parent->flags & 0xFFU) != VFS_DIRECTORY) return -1;
    if (vfs_find_entry(parent, name)) return -1;
    struct vfs_node *link = vfs_attach_link(parent, name, target);
    if (!link) return -1;

    vfs_stamp_times(target, VFS_TIME_CTIME);
    inotify_notify(parent, TUNIX_IN_CREATE, name, 0);
    PERSIST(linked, link);
    return 0;
}

int64_t vfs_readlink(struct vfs_node *node, void *buffer, size_t size) {
    if (!node || !buffer || (node->flags & 0xFFU) != VFS_SYMLINK || !node->data) return -1;
    size_t length = (size_t)node->length;
    if (length > size) length = size;
    memcpy(buffer, node->data, length);
    return (int64_t)length;
}

/*
 * Called once the node is already detached from its parent. A node can still be
 * somebody's current working directory at that point -- Linux lets you rmdir a
 * directory a process is sitting in -- so freeing unconditionally would leave
 * that process with a dangling cwd. Instead the node is marked orphaned and the
 * last vfs_node_unref() finishes the job.
 */
static void destroy_node(struct vfs_node *node) {
    if (!node) return;
    /* A hard link owns nothing but its name, so it goes on its own. Dropping
       the last name of a node that has already lost its own is what finally
       releases the contents -- and what tells the filesystem to free them. */
    if (node->link_target) {
        struct vfs_node *target = node->link_target;
        node->link_target = NULL;
        kfree(node);
        if (target->links) target->links--;
        if (!target->links && (target->flags & VFS_ORPHANED))
            PERSIST(released, target);
        vfs_node_unref(target);
        return;
    }
    if (node->links) node->links--;
    if (node->refs) {
        node->flags |= VFS_ORPHANED;
        return;
    }
    free_node_data(node);
    kfree(node);
}

void vfs_node_ref(struct vfs_node *node) {
    if (node) node->refs++;
}

void vfs_node_unref(struct vfs_node *node) {
    if (!node || !node->refs) return;
    if (--node->refs) return;
    /* Still linked into the tree: the parent owns it, nothing to do. */
    if (!(node->flags & VFS_ORPHANED)) return;
    free_node_data(node);
    kfree(node);
}

static int detach_child(struct vfs_node *parent, struct vfs_node *node) {
    if (!parent || !node) return -1;
    struct vfs_node *previous = NULL;
    for (struct vfs_node *item = parent->children; item; item = item->next) {
        if (item == node) {
            if (previous) previous->next = item->next;
            else parent->children = item->next;
            item->next = NULL;
            item->parent = NULL;
            return 0;
        }
        previous = item;
    }
    return -1;
}

int vfs_detach_child(struct vfs_node *parent, struct vfs_node *node) {
    if (!parent || !node) return -1;
    if (detach_child(parent, node) != 0) return -1;
    destroy_node(node);
    return 0;
}

int vfs_remove(const char *path, int remove_directory) {
    char parent_path[256];
    char name[128];
    if (split_parent(path, parent_path, name) != 0) return -1;
    struct vfs_node *parent = vfs_lookup(parent_path);
    if (!parent || (parent->flags & 0xFFU) != VFS_DIRECTORY) return -1;
    struct vfs_node *node = vfs_find_entry(parent, name);
    if (!node) return -1;
    struct vfs_node *body = node->link_target ? node->link_target : node;
    if (body->flags & VFS_READONLY) return -1;
    uint32_t kind = node->flags & 0xFFU;
    if (remove_directory) {
        if (kind != VFS_DIRECTORY || node->children) return -1;
    } else if (kind == VFS_DIRECTORY) return -1;
    inotify_notify(parent, TUNIX_IN_DELETE, name, 0);
    /* Only the last name takes the contents with it; watchers of a file that
       still has another name have not seen it deleted. */
    if (body->links <= 1) {
        inotify_notify(body, TUNIX_IN_DELETE_SELF, NULL, 0);
        inotify_invalidate(body);
    }
    PERSIST(removed, node);
    if (detach_child(parent, node) != 0) return -1;
    destroy_node(node);
    return 0;
}

int vfs_rename(const char *old_path, const char *new_path) {
    char old_parent_path[256], old_name[128];
    char new_parent_path[256], new_name[128];
    if (split_parent(old_path, old_parent_path, old_name) != 0 ||
        split_parent(new_path, new_parent_path, new_name) != 0) return -1;
    struct vfs_node *old_parent = vfs_lookup(old_parent_path);
    struct vfs_node *new_parent = vfs_lookup(new_parent_path);
    if (!old_parent || !new_parent ||
        (old_parent->flags & 0xFFU) != VFS_DIRECTORY ||
        (new_parent->flags & 0xFFU) != VFS_DIRECTORY) return -1;
    struct vfs_node *node = vfs_find_entry(old_parent, old_name);
    if (!node) return -1;
    if ((node->link_target ? node->link_target : node)->flags & VFS_READONLY)
        return -1;

    struct vfs_node *existing = vfs_find_entry(new_parent, new_name);
    if (existing && existing != node) {
        if ((existing->link_target ? existing->link_target : existing)->flags &
            VFS_READONLY) return -1;
        uint32_t existing_kind = existing->flags & 0xFFU;
        uint32_t node_kind = node->flags & 0xFFU;
        if ((existing_kind == VFS_DIRECTORY) != (node_kind == VFS_DIRECTORY)) return -1;
        if (existing_kind == VFS_DIRECTORY && existing->children) return -1;
        struct vfs_node *body =
            existing->link_target ? existing->link_target : existing;
        inotify_notify(new_parent, TUNIX_IN_DELETE, new_name, 0);
        if (body->links <= 1) {
            inotify_notify(body, TUNIX_IN_DELETE_SELF, NULL, 0);
            inotify_invalidate(body);
        }
        PERSIST(removed, existing);
        if (detach_child(new_parent, existing) != 0) return -1;
        destroy_node(existing);
    }
    if (old_parent == new_parent && strcmp(old_name, new_name) == 0) return 0;
    uint32_t cookie = inotify_next_cookie();
    inotify_notify(old_parent, TUNIX_IN_MOVED_FROM, old_name, cookie);
    inotify_notify(new_parent, TUNIX_IN_MOVED_TO, new_name, cookie);
    inotify_notify(node->link_target ? node->link_target : node,
                   TUNIX_IN_MOVE_SELF, NULL, cookie);
    if (detach_child(old_parent, node) != 0) return -1;
    strncpy(node->name, new_name, sizeof(node->name) - 1);
    node->name[sizeof(node->name) - 1] = '\0';
    if (vfs_attach(new_parent, node) != 0) return -1;
    PERSIST(moved, node, old_parent, old_name);
    return 0;
}

/* --- mounts -------------------------------------------------------------- */

/* Errno values, so a mount failure can say which one it was. Kept here rather
   than taken from the syscall layer, which the VFS does not include. */
#define VFS_EPERM   1
#define VFS_ENOENT  2
#define VFS_ENOMEM 12
#define VFS_EBUSY  16
#define VFS_ENODEV 19
#define VFS_ENOTDIR 20
#define VFS_EINVAL 22

static struct vfs_mount *mount_table;

const struct vfs_mount *vfs_mounts(void) { return mount_table; }

static void mount_field(char *out, size_t capacity, const char *value) {
    strncpy(out, value ? value : "", capacity - 1);
    out[capacity - 1] = '\0';
}

static struct vfs_mount *mount_at(const char *target) {
    for (struct vfs_mount *entry = mount_table; entry; entry = entry->next)
        if (strcmp(entry->target, target) == 0) return entry;
    return NULL;
}

/* Appended, so /proc/mounts reads in the order the system came up -- except
   the root, which goes first however late it is declared, because that is
   where every reader of the file expects to find it. */
static void mount_insert(struct vfs_mount *entry) {
    if (strcmp(entry->target, "/") == 0) {
        entry->next = mount_table;
        mount_table = entry;
        return;
    }
    struct vfs_mount **at = &mount_table;
    while (*at) at = &(*at)->next;
    *at = entry;
}

void vfs_mount_builtin(const char *source, const char *target, const char *type,
                       struct vfs_node *root) {
    if (!target || mount_at(target)) return;
    struct vfs_mount *entry = (struct vfs_mount *)kmalloc(sizeof(*entry));
    if (!entry) return;
    memset(entry, 0, sizeof(*entry));
    mount_field(entry->source, sizeof(entry->source), source);
    mount_field(entry->target, sizeof(entry->target), target);
    mount_field(entry->type, sizeof(entry->type), type);
    entry->root = root;
    mount_insert(entry);
}

/* Tear down a tree nobody can reach any more. Nodes a process still holds --
   its working directory, say -- are orphaned by destroy_node rather than
   freed under it, exactly as an unlink would leave them. */
static void free_tree(struct vfs_node *node, unsigned depth) {
    if (!node || depth > VFS_TREE_MAX_DEPTH) return;
    struct vfs_node *child = node->children;
    node->children = NULL;
    while (child) {
        struct vfs_node *next = child->next;
        child->next = NULL;
        child->parent = NULL;
        free_tree(child, depth + 1U);
        child = next;
    }
    destroy_node(node);
}

static int mount_is_pseudo(const char *type) {
    return strcmp(type, "proc") == 0 || strcmp(type, "sysfs") == 0 ||
           strcmp(type, "devtmpfs") == 0 || strcmp(type, "devfs") == 0;
}

int vfs_mount(const char *source, const char *target, const char *type,
              uint32_t flags) {
    if (!target || !type || target[0] != '/') return -VFS_EINVAL;
    if (flags & ~VFS_MS_SUPPORTED) return -VFS_EINVAL;

    struct vfs_mount *existing = mount_at(target);
    if (flags & VFS_MS_REMOUNT) {
        if (!existing) return -VFS_EINVAL;
        existing->flags = flags & ~VFS_MS_REMOUNT;
        return 0;
    }
    /* Mounting a second filesystem over the first would give two entries the
       same target, and then umount could not say which it meant. */
    if (existing) return -VFS_EBUSY;

    struct vfs_node *at = vfs_lookup(target);
    if (!at) return -VFS_ENOENT;
    if ((at->flags & 0xFFU) != VFS_DIRECTORY) return -VFS_ENOTDIR;
    if (at == vfs_root) return -VFS_EBUSY;
    if (at->mounted) return -VFS_EBUSY;

    struct vfs_node *root = NULL;
    int owns_root = 0;

    if (flags & VFS_MS_BIND) {
        if (!source || source[0] != '/') return -VFS_EINVAL;
        root = vfs_lookup(source);
        if (!root) return -VFS_ENOENT;
        if ((root->flags & 0xFFU) != VFS_DIRECTORY) return -VFS_ENOTDIR;
        if (root == at) return -VFS_EBUSY;
    } else if (strcmp(type, "tmpfs") == 0 || strcmp(type, "ramfs") == 0) {
        /* A fresh, empty tree. Volatile so the ext2 driver never writes any of
           it to the disk, which is what makes it a tmpfs rather than a
           directory that happens to be empty. */
        root = vfs_alloc_node(at->name, VFS_DIRECTORY | VFS_VOLATILE);
        if (!root) return -VFS_ENOMEM;
        root->mode = at->mode;
        root->uid = at->uid;
        root->gid = at->gid;
        owns_root = 1;
    } else if (strcmp(type, "vfat") == 0 || strcmp(type, "fat") == 0 ||
               strcmp(type, "msdos") == 0) {
        /* The one filesystem here that reads a device. Its tree is built now,
           in full, because FAT directories are small and a lookup that had to
           re-read the medium would pay for it on every path walk. */
        int status = fatfs_mount(source, at->name, &root);
        if (status != 0) return status;
        owns_root = 1;
    } else if (mount_is_pseudo(type)) {
        /* These trees are built by their own drivers at boot and cannot be
           made a second time; mounting one is only meaningful where it
           already is, which the table above has already answered. */
        return -VFS_EBUSY;
    } else {
        /* ext2 and everything else: no driver here can open a device. */
        return -VFS_ENODEV;
    }

    struct vfs_mount *entry = (struct vfs_mount *)kmalloc(sizeof(*entry));
    if (!entry) {
        if (owns_root) free_tree(root, 0);
        return -VFS_ENOMEM;
    }
    memset(entry, 0, sizeof(*entry));
    mount_field(entry->source, sizeof(entry->source), source ? source : type);
    mount_field(entry->target, sizeof(entry->target), target);
    mount_field(entry->type, sizeof(entry->type), (flags & VFS_MS_BIND) ? "bind" : type);
    entry->flags = flags;
    entry->mountpoint = at;
    entry->root = root;
    entry->owns_root = owns_root;

    /* The mounted root stands in for the mountpoint, so it takes the
       mountpoint's name and its *parent* -- which is what makes `..` leave the
       mount and vfs_node_path spell the path the caller walked. */
    if (owns_root) {
        root->parent = at->parent;
        strncpy(root->name, at->name, sizeof(root->name) - 1);
        root->name[sizeof(root->name) - 1] = '\0';
    }
    at->mounted = root;
    at->flags |= VFS_MOUNTPOINT;
    mount_insert(entry);
    return 0;
}

int vfs_umount(const char *target) {
    if (!target) return -VFS_EINVAL;
    struct vfs_mount *previous = NULL;
    struct vfs_mount *entry = mount_table;
    while (entry && strcmp(entry->target, target) != 0) {
        previous = entry;
        entry = entry->next;
    }
    if (!entry) return -VFS_EINVAL;
    /* The trees the system booted with have no mountpoint to restore. */
    if (!entry->mountpoint) return -VFS_EPERM;

    entry->mountpoint->mounted = NULL;
    entry->mountpoint->flags &= ~VFS_MOUNTPOINT;
    if (previous) previous->next = entry->next;
    else mount_table = entry->next;
    /* A FAT tree carries per-node state the VFS knows nothing about, so its
       driver gets to let go before the nodes themselves are freed. */
    fatfs_unmount(entry->root);
    if (entry->owns_root) free_tree(entry->root, 0);
    kfree(entry);
    return 0;
}

int vfs_truncate(struct vfs_node *node, uint64_t length) {
    if (!node || (node->flags & 0xFFU) != VFS_FILE || (node->flags & VFS_READONLY)) return -1;
    if (node->truncate) {
        if (node->truncate(node, length) != 0) return -1;
        node->length = length;
        vfs_stamp_times(node, VFS_TIME_MTIME | VFS_TIME_CTIME);
        inotify_notify(node, TUNIX_IN_MODIFY, NULL, 0);
        return 0;
    }
    /* Truncating to nothing discards the contents, so drop the promise
       instead of paying to fulfil it. */
    if (!length) node->flags &= ~VFS_LAZY_DATA;
    else if (vfs_fault_in(node) != 0) return -1;
    if (ensure_capacity(node, length) != 0) return -1;
    if (length > node->length) memset((uint8_t *)node->data + node->length, 0, (size_t)(length - node->length));
    node->length = length;
    vfs_stamp_times(node, VFS_TIME_MTIME | VFS_TIME_CTIME);
    inotify_notify(node, TUNIX_IN_MODIFY, NULL, 0);
    PERSIST(truncated, node);
    return 0;
}

int64_t vfs_read(struct vfs_node *node, uint64_t offset, size_t size, void *buffer) {
    if (!node || !node->read) return -1;
    return node->read(node, offset, size, buffer);
}

int64_t vfs_write(struct vfs_node *node, uint64_t offset, size_t size, const void *buffer) {
    if (!node || !node->write) return -1;
    return node->write(node, offset, size, buffer);
}

int vfs_readdir(struct vfs_node *directory, uint64_t index, struct dirent *out) {
    if (!directory || !out || (directory->flags & 0xFFU) != VFS_DIRECTORY) return -1;
    if (directory->refresh) directory->refresh(directory);
    struct vfs_node *node = directory->children;
    while (node && index--) node = node->next;
    if (!node) return 0;
    memset(out, 0, sizeof(*out));
    strncpy(out->name, node->name, sizeof(out->name) - 1);
    /* Two names for one file have to report one inode, or nothing looking for
       hard links -- tar, cp -l, du -- can pair them up. */
    out->ino = node->link_target ? node->link_target->inode : node->inode;
    out->type = node->flags & 0xFFU;
    return 1;
}
