#include <stddef.h>
#include <stdint.h>
#include "../include/cred.h"
#include "../include/heap.h"
#include "../include/pmm.h"
#include "../include/vmm.h"
#include "../include/inotify.h"
#include "../include/kstring.h"
#include "../include/time.h"
#include "../include/fatfs.h"
#include "../include/pipe.h"
#include "../include/process.h"
#include "../include/vfs.h"

extern void kprintf(const char *fmt, ...);

#define VFS_PATH_MAX 256
#define VFS_SYMLINK_MAX_DEPTH 16
#define VFS_MOUNT_MAX_DEPTH 16
#define VFS_TREE_MAX_DEPTH 64

struct vfs_node *vfs_root;
static uint64_t next_inode = 1;
static const struct vfs_persist_ops *persist_ops;

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

static uint64_t cached_bytes;

static void page_free_one(struct vfs_page_map *map, uint64_t index);
static void page_map_free(struct vfs_node *node);
static uint64_t reclaim_below(struct vfs_node *node, uint32_t newer_than);

static int cacheable(const struct vfs_node *node) {
    return node && (node->flags & 0xFFU) == VFS_FILE && node->disk_inode &&
           node->pages && node->pages->resident;
}

uint64_t vfs_cached_bytes(void) { return cached_bytes; }

int vfs_fault_in(struct vfs_node *node) {
    if (!node) return -1;
    node->flags &= ~VFS_LAZY_DATA;
    return 0;
}

void vfs_map_ref(struct vfs_node *node) {
    if (node) node->mapped_refs++;
}

void vfs_map_unref(struct vfs_node *node) {
    if (node && node->mapped_refs) node->mapped_refs--;
}

void vfs_map_write_ref(struct vfs_node *node, uint64_t offset, uint64_t length) {
    if (!node) return;
    node->shared_writers++;
    uint64_t end = offset + length;
    if (end < offset) end = UINT64_MAX;
    if (!node->map_dirty_end) {
        node->map_dirty_start = offset;
        node->map_dirty_end = end;
        return;
    }
    if (offset < node->map_dirty_start) node->map_dirty_start = offset;
    if (end > node->map_dirty_end) node->map_dirty_end = end;
}

void vfs_flush_mapped(struct vfs_node *node) {
    if (!node || !node->map_dirty_end) return;
    uint64_t start = node->map_dirty_start;
    uint64_t end = node->map_dirty_end;
    node->map_dirty_start = 0;
    node->map_dirty_end = 0;
    if (end > node->length) end = node->length;
    if (start >= end) return;
    for (uint64_t index = start / VFS_PAGE_SIZE;
         index <= (end - 1ULL) / VFS_PAGE_SIZE; index++) {
        if (vfs_page_peek(node, index) && node->pages)
            node->pages->dirty[index / 64ULL] |= 1ULL << (index % 64ULL);
    }
    vfs_stamp_times(node, VFS_TIME_MTIME | VFS_TIME_CTIME);
    inotify_notify(node, TUNIX_IN_MODIFY, NULL, 0);
    PERSIST(written, node, start, end - start);
}

void vfs_map_write_unref(struct vfs_node *node) {
    if (!node || !node->shared_writers) return;
    if (--node->shared_writers == 0) vfs_flush_mapped(node);
}

uint64_t vfs_drop_clean_pages(struct vfs_node *node) {
    if (!cacheable(node) || node->mapped_refs) return 0;
    if (!persist_ops || !persist_ops->fetch_page) return 0;
    struct vfs_page_map *map = node->pages;
    uint64_t dropped = 0;
    for (uint64_t index = 0; index < map->count; index++) {
        if (!map->page[index] || vfs_page_is_dirty(node, index)) continue;
        page_free_one(map, index);
        cached_bytes -= cached_bytes >= VFS_PAGE_SIZE ? VFS_PAGE_SIZE : cached_bytes;
        dropped += VFS_PAGE_SIZE;
    }
    return dropped;
}

void vfs_release_data(struct vfs_node *node) {
    vfs_flush_mapped(node);
    (void)vfs_drop_clean_pages(node);
}

static uint64_t reclaim_below(struct vfs_node *node, uint32_t newer_than) {
    if (!node || node->link_target) return 0;

    uint64_t reclaimed = 0;
    uint32_t touched = node->atime > node->mtime ? node->atime : node->mtime;
    if ((node->flags & 0xFFU) == VFS_FILE && touched < newer_than)
        reclaimed += vfs_drop_clean_pages(node);

    for (struct vfs_node *child = node->children; child; child = child->next)
        reclaimed += reclaim_below(child, newer_than);

    return reclaimed;
}

uint64_t vfs_reclaim_file_data(struct vfs_node *node) {
    return reclaim_below(node, 0xFFFFFFFFU);
}

void vfs_trim_cache(uint64_t budget) {
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

static void free_node_data(struct vfs_node *node) {
    if (!node || node->mapped_refs) return;
    if (node->pages) {
        uint64_t held = node->disk_inode ? node->pages->resident * VFS_PAGE_SIZE : 0;
        page_map_free(node);
        cached_bytes -= cached_bytes >= held ? held : cached_bytes;
    }
    if (!node->data || !(node->flags & VFS_OWNED_DATA)) return;
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

static struct vfs_node *cross_mounts(struct vfs_node *node) {
    unsigned depth = 0;
    while (node && node->mounted && depth++ < VFS_MOUNT_MAX_DEPTH)
        node = node->mounted;
    return node;
}

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
            if (current != process_get_root() && current->parent) current = current->parent;
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
                struct vfs_node *root = process_get_root();
                if (root && root != vfs_root) {
                    char root_path[VFS_PATH_MAX];
                    if (vfs_node_path(root, root_path, sizeof(root_path)) != 0) return NULL;
                    size_t root_length = strlen(root_path);
                    while (root_length > 1 && root_path[root_length - 1] == '/')
                        root_path[--root_length] = '\0';
                    if (root_length > 1 &&
                        append_text(resolved, sizeof(resolved), &at, root_path) != 0) return NULL;
                }
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

static uint64_t pages_for(uint64_t length) {
    return (length + VFS_PAGE_SIZE - 1ULL) / VFS_PAGE_SIZE;
}

static void page_free_one(struct vfs_page_map *map, uint64_t index) {
    if (!map->page[index]) return;
    uint64_t physical = vmm_virt_to_phys_direct(map->page[index]);
    map->page[index] = NULL;
    map->dirty[index / 64ULL] &= ~(1ULL << (index % 64ULL));
    if (map->resident) map->resident--;
    if (physical) pmm_free_page((void *)physical);
}

static void page_map_free(struct vfs_node *node) {
    struct vfs_page_map *map = node->pages;
    if (!map) return;
    for (uint64_t index = 0; index < map->count; index++) page_free_one(map, index);
    kfree(map->page);
    kfree(map->dirty);
    kfree(map);
    node->pages = NULL;
}

static struct vfs_page_map *page_map_grow(struct vfs_node *node, uint64_t needed) {
    struct vfs_page_map *map = node->pages;
    if (!map) {
        map = (struct vfs_page_map *)kmalloc(sizeof(*map));
        if (!map) return NULL;
        memset(map, 0, sizeof(*map));
        node->pages = map;
    }
    if (needed <= map->count) return map;

    uint64_t count = map->count ? map->count : 8ULL;
    while (count < needed) {
        if (count > UINT64_MAX / 2ULL) return NULL;
        count *= 2ULL;
    }
    uint64_t words = (count + 63ULL) / 64ULL;
    uint8_t **page = (uint8_t **)kmalloc((size_t)(count * sizeof(uint8_t *)));
    uint64_t *dirty = (uint64_t *)kmalloc((size_t)(words * sizeof(uint64_t)));
    if (!page || !dirty) {
        kfree(page);
        kfree(dirty);
        return NULL;
    }
    memset(page, 0, (size_t)(count * sizeof(uint8_t *)));
    memset(dirty, 0, (size_t)(words * sizeof(uint64_t)));
    if (map->count) {
        memcpy(page, map->page, (size_t)(map->count * sizeof(uint8_t *)));
        memcpy(dirty, map->dirty,
               (size_t)(((map->count + 63ULL) / 64ULL) * sizeof(uint64_t)));
        kfree(map->page);
        kfree(map->dirty);
    }
    map->page = page;
    map->dirty = dirty;
    map->count = count;
    return map;
}

static uint8_t *page_allocate(struct vfs_node *node, struct vfs_page_map *map,
                              uint64_t index) {
    void *physical = pmm_alloc_page();
    if (!physical) {
        if (reclaim_below(vfs_root, (uint32_t)time_epoch_seconds()))
            physical = pmm_alloc_page();
    }
    if (!physical) return NULL;
    uint8_t *page = (uint8_t *)vmm_phys_to_virt((uint64_t)physical);
    memset(page, 0, (size_t)VFS_PAGE_SIZE);
    map->page[index] = page;
    map->resident++;
    if (node->disk_inode) cached_bytes += VFS_PAGE_SIZE;
    return page;
}

void *vfs_page(struct vfs_node *node, uint64_t index, int for_write) {
    if (!node) return NULL;
    if (!for_write) {
        uint64_t span = pages_for(node->length);
        if (index >= span) return NULL;
    }
    struct vfs_page_map *map = page_map_grow(node, index + 1ULL);
    if (!map) return NULL;
    if (!map->page[index]) {
        if (!page_allocate(node, map, index)) return NULL;
        if (node->disk_inode && persist_ops && persist_ops->fetch_page &&
            index < pages_for(node->length)) {
            if (persist_ops->fetch_page(node, index, map->page[index]) != 0) {
                page_free_one(map, index);
                return NULL;
            }
        }
    }
    if (for_write) map->dirty[index / 64ULL] |= 1ULL << (index % 64ULL);
    return map->page[index];
}

void *vfs_page_peek(struct vfs_node *node, uint64_t index) {
    if (!node || !node->pages || index >= node->pages->count) return NULL;
    return node->pages->page[index];
}

uint64_t vfs_page_physical(struct vfs_node *node, uint64_t index) {
    void *page = vfs_page(node, index, 0);
    if (!page) return 0;
    return vmm_virt_to_phys_direct(page);
}

int vfs_page_is_dirty(struct vfs_node *node, uint64_t index) {
    if (!node || !node->pages || index >= node->pages->count) return 0;
    return (node->pages->dirty[index / 64ULL] >> (index % 64ULL)) & 1ULL;
}

void vfs_page_clear_dirty(struct vfs_node *node, uint64_t index) {
    if (!node || !node->pages || index >= node->pages->count) return;
    node->pages->dirty[index / 64ULL] &= ~(1ULL << (index % 64ULL));
}

uint64_t vfs_page_span(struct vfs_node *node) {
    if (!node) return 0;
    uint64_t span = pages_for(node->length);
    if (node->pages && node->pages->count > span) span = node->pages->count;
    return span;
}

static int64_t memory_read(struct vfs_node *node, uint64_t offset, size_t size, void *buffer) {
    if (!node || !buffer || offset >= node->length) return 0;
    uint64_t available = node->length - offset;
    if ((uint64_t)size > available) size = (size_t)available;

    uint8_t *out = (uint8_t *)buffer;
    uint64_t at = offset;
    uint64_t left = size;
    while (left) {
        uint64_t index = at / VFS_PAGE_SIZE;
        uint64_t within = at % VFS_PAGE_SIZE;
        uint64_t chunk = VFS_PAGE_SIZE - within;
        if (chunk > left) chunk = left;
        const uint8_t *page = (const uint8_t *)vfs_page(node, index, 0);
        if (!page) return (int64_t)(size - left);
        memcpy(out, page + within, (size_t)chunk);
        out += chunk;
        at += chunk;
        left -= chunk;
    }
    if (size) vfs_stamp_times(node, VFS_TIME_ATIME);
    return (int64_t)size;
}

static int64_t memory_write(struct vfs_node *node, uint64_t offset, size_t size, const void *buffer) {
    if (!node || !buffer || (node->flags & VFS_READONLY)) return -1;
    if ((uint64_t)size > UINT64_MAX - offset) return -1;
    uint64_t end = offset + size;

    const uint8_t *in = (const uint8_t *)buffer;
    uint64_t at = offset;
    uint64_t left = size;
    while (left) {
        uint64_t index = at / VFS_PAGE_SIZE;
        uint64_t within = at % VFS_PAGE_SIZE;
        uint64_t chunk = VFS_PAGE_SIZE - within;
        if (chunk > left) chunk = left;
        uint8_t *page = (uint8_t *)vfs_page(node, index, 1);
        if (!page) {
            if (at > node->length) node->length = at;
            return at > offset ? (int64_t)(at - offset) : -1;
        }
        memcpy(page + within, in, (size_t)chunk);
        in += chunk;
        at += chunk;
        left -= chunk;
    }
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
    node->length = 0;
    node->capacity = 0;
    if (length) {
        uint64_t stored = 0;
        while (stored < length) {
            uint64_t index = stored / VFS_PAGE_SIZE;
            uint64_t chunk = VFS_PAGE_SIZE;
            if (chunk > length - stored) chunk = length - stored;
            uint8_t *page = (uint8_t *)vfs_page(node, index, 1);
            if (!page) {
                free_node_data(node);
                kfree(node);
                return NULL;
            }
            memcpy(page, (const uint8_t *)data + stored, (size_t)chunk);
            stored += chunk;
        }
        node->length = length;
    }
    (void)copy_data;
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

struct vfs_node *vfs_create_fifo(const char *path, uint32_t mode) {
    char parent_path[256];
    char name[128];
    if (split_parent(path, parent_path, name) != 0) return NULL;
    struct vfs_node *parent = vfs_lookup(parent_path);
    if (!parent || (parent->flags & 0xFFU) != VFS_DIRECTORY) return NULL;
    if (vfs_find_child(parent, name)) return NULL;

    struct vfs_node *node = vfs_alloc_node(name, VFS_PIPE | VFS_VOLATILE);
    if (!node) return NULL;
    node->mode = mode & 07777U;
    if (vfs_attach(parent, node) != 0) {
        kfree(node);
        return NULL;
    }
    cred_stamp_new_node(node);
    node->mode = mode & 07777U;
    inotify_notify(parent, TUNIX_IN_CREATE, name, 0);
    return node;
}

struct vfs_node *vfs_create_socket_node(const char *path, uint32_t mode) {
    char parent_path[256];
    char name[128];
    if (split_parent(path, parent_path, name) != 0) return NULL;
    struct vfs_node *parent = vfs_lookup(parent_path);
    if (!parent || (parent->flags & 0xFFU) != VFS_DIRECTORY) return NULL;
    if (vfs_find_child(parent, name)) return NULL;

    struct vfs_node *node = vfs_alloc_node(name, VFS_SOCKET | VFS_VOLATILE);
    if (!node) return NULL;
    node->mode = mode & 07777U;
    if (vfs_attach(parent, node) != 0) {
        kfree(node);
        return NULL;
    }
    cred_stamp_new_node(node);
    node->mode = mode & 07777U;
    inotify_notify(parent, TUNIX_IN_CREATE, name, 0);
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

    struct vfs_node *link = vfs_alloc_node(name, kind | VFS_HARDLINK);
    if (!link) return NULL;
    link->link_target = target;
    if (vfs_attach(parent, link) != 0) {
        kfree(link);
        return NULL;
    }
    target->links++;
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

static void destroy_node(struct vfs_node *node) {
    if (!node) return;
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
    if (node->fifo) {
        pipe_buffer_destroy(node->fifo);
        node->fifo = NULL;
    }
    kfree(node);
}

void vfs_node_ref(struct vfs_node *node) {
    if (node) node->refs++;
}

void vfs_node_unref(struct vfs_node *node) {
    if (!node || !node->refs) return;
    if (--node->refs) return;
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
           strcmp(type, "devtmpfs") == 0 || strcmp(type, "devfs") == 0 ||
           strcmp(type, "eventfs") == 0;
}

int vfs_mount(const char *source, const char *target, const char *type,
              uint32_t flags) {
    if (!target || !type || target[0] != '/') return -VFS_EINVAL;
    if (flags & ~(VFS_MS_SUPPORTED | VFS_MS_IGNORED)) return -VFS_EINVAL;
    flags &= ~VFS_MS_IGNORED;

    struct vfs_mount *existing = mount_at(target);
    if (flags & VFS_MS_REMOUNT) {
        if (!existing) return -VFS_EINVAL;
        existing->flags = flags & ~VFS_MS_REMOUNT;
        return 0;
    }
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
        root = vfs_alloc_node(at->name, VFS_DIRECTORY | VFS_VOLATILE);
        if (!root) return -VFS_ENOMEM;
        root->mode = at->mode;
        root->uid = at->uid;
        root->gid = at->gid;
        owns_root = 1;
    } else if (strcmp(type, "vfat") == 0 || strcmp(type, "fat") == 0 ||
               strcmp(type, "msdos") == 0) {
        int status = fatfs_mount(source, at->name, &root);
        if (status != 0) return status;
        owns_root = 1;
    } else if (mount_is_pseudo(type)) {
        return -VFS_EBUSY;
    } else {
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
    if (!entry->mountpoint) return -VFS_EPERM;

    entry->mountpoint->mounted = NULL;
    entry->mountpoint->flags &= ~VFS_MOUNTPOINT;
    if (previous) previous->next = entry->next;
    else mount_table = entry->next;
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
    if (length < node->length && node->pages) {
        uint64_t keep = (length + VFS_PAGE_SIZE - 1ULL) / VFS_PAGE_SIZE;
        for (uint64_t index = keep; index < node->pages->count; index++) {
            if (node->pages->page[index]) {
                page_free_one(node->pages, index);
                if (node->disk_inode)
                    cached_bytes -= cached_bytes >= VFS_PAGE_SIZE ? VFS_PAGE_SIZE : cached_bytes;
            }
        }
        if (length % VFS_PAGE_SIZE) {
            uint8_t *tail = (uint8_t *)vfs_page_peek(node, length / VFS_PAGE_SIZE);
            if (tail) memset(tail + length % VFS_PAGE_SIZE, 0,
                             (size_t)(VFS_PAGE_SIZE - length % VFS_PAGE_SIZE));
        }
    }
    node->flags &= ~VFS_LAZY_DATA;
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
    out->ino = node->link_target ? node->link_target->inode : node->inode;
    out->type = node->flags & 0xFFU;
    return 1;
}
