#include <stddef.h>
#include <stdint.h>
#include "include/heap.h"
#include "include/kstring.h"
#include "include/vfs.h"

#define VFS_PATH_MAX 256
#define VFS_SYMLINK_MAX_DEPTH 16

struct vfs_node *vfs_root;
static uint64_t next_inode = 1;

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
    uint32_t kind = flags & 0xFFU;
    node->mode = kind == VFS_DIRECTORY ? 0755 : (kind == VFS_SYMLINK ? 0777 : 0644);
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

struct vfs_node *vfs_find_child(struct vfs_node *directory, const char *name) {
    if (!directory || !name || (directory->flags & 0xFFU) != VFS_DIRECTORY) return NULL;
    for (struct vfs_node *node = directory->children; node; node = node->next) {
        if (strcmp(node->name, name) == 0) return node;
    }
    return NULL;
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
    if (path[1] == '\0') return vfs_root;

    struct vfs_node *current = vfs_root;
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
    if (!node || !buffer || offset >= node->length) return 0;
    uint64_t available = node->length - offset;
    if ((uint64_t)size > available) size = (size_t)available;
    memcpy(buffer, (const uint8_t *)node->data + offset, size);
    return (int64_t)size;
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
    if ((node->flags & VFS_OWNED_DATA) && node->data) kfree(node->data);
    node->data = new_data;
    node->capacity = capacity;
    node->flags |= VFS_OWNED_DATA;
    return 0;
}

static int64_t memory_write(struct vfs_node *node, uint64_t offset, size_t size, const void *buffer) {
    if (!node || !buffer || (node->flags & VFS_READONLY)) return -1;
    if ((uint64_t)size > UINT64_MAX - offset) return -1;
    uint64_t end = offset + size;
    if (ensure_capacity(node, end) != 0) return -1;
    memcpy((uint8_t *)node->data + offset, buffer, size);
    if (end > node->length) node->length = end;
    return (int64_t)size;
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
        if ((node->flags & VFS_OWNED_DATA) && node->data) kfree(node->data);
        kfree(node);
        return NULL;
    }
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
    return node;
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
    if ((node->flags & VFS_OWNED_DATA) && node->data) kfree(node->data);
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

int vfs_remove(const char *path, int remove_directory) {
    char parent_path[256];
    char name[128];
    if (split_parent(path, parent_path, name) != 0) return -1;
    struct vfs_node *parent = vfs_lookup(parent_path);
    if (!parent || (parent->flags & 0xFFU) != VFS_DIRECTORY) return -1;
    struct vfs_node *node = vfs_find_child(parent, name);
    if (!node || (node->flags & VFS_READONLY)) return -1;
    uint32_t kind = node->flags & 0xFFU;
    if (remove_directory) {
        if (kind != VFS_DIRECTORY || node->children) return -1;
    } else if (kind == VFS_DIRECTORY) return -1;
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
    struct vfs_node *node = vfs_find_child(old_parent, old_name);
    if (!node || (node->flags & VFS_READONLY)) return -1;

    struct vfs_node *existing = vfs_find_child(new_parent, new_name);
    if (existing && existing != node) {
        if (existing->flags & VFS_READONLY) return -1;
        uint32_t existing_kind = existing->flags & 0xFFU;
        uint32_t node_kind = node->flags & 0xFFU;
        if ((existing_kind == VFS_DIRECTORY) != (node_kind == VFS_DIRECTORY)) return -1;
        if (existing_kind == VFS_DIRECTORY && existing->children) return -1;
        if (detach_child(new_parent, existing) != 0) return -1;
        destroy_node(existing);
    }
    if (old_parent == new_parent && strcmp(old_name, new_name) == 0) return 0;
    if (detach_child(old_parent, node) != 0) return -1;
    strncpy(node->name, new_name, sizeof(node->name) - 1);
    node->name[sizeof(node->name) - 1] = '\0';
    if (vfs_attach(new_parent, node) != 0) return -1;
    return 0;
}

int vfs_truncate(struct vfs_node *node, uint64_t length) {
    if (!node || (node->flags & 0xFFU) != VFS_FILE || (node->flags & VFS_READONLY)) return -1;
    if (ensure_capacity(node, length) != 0) return -1;
    if (length > node->length) memset((uint8_t *)node->data + node->length, 0, (size_t)(length - node->length));
    node->length = length;
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
    struct vfs_node *node = directory->children;
    while (node && index--) node = node->next;
    if (!node) return 0;
    memset(out, 0, sizeof(*out));
    strncpy(out->name, node->name, sizeof(out->name) - 1);
    out->ino = node->inode;
    out->type = node->flags & 0xFFU;
    return 1;
}
