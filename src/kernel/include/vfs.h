#ifndef TUNIX_VFS_H
#define TUNIX_VFS_H

#include <stddef.h>
#include <stdint.h>

#define VFS_FILE        0x01U
#define VFS_DIRECTORY   0x02U
#define VFS_CHARDEVICE  0x03U
#define VFS_BLOCKDEVICE 0x04U
#define VFS_PIPE        0x05U
#define VFS_SYMLINK     0x06U
#define VFS_READONLY    0x100U
#define VFS_OWNED_DATA  0x200U
#define VFS_INPUTDEVICE 0x400U
#define VFS_FRAMEBUFFER 0x800U
#define VFS_VOLATILE    0x1000U

struct vfs_node;
struct file;

typedef int64_t (*vfs_read_fn)(struct vfs_node *, uint64_t, size_t, void *);
typedef int64_t (*vfs_write_fn)(struct vfs_node *, uint64_t, size_t, const void *);
typedef int64_t (*vfs_ioctl_fn)(struct vfs_node *, unsigned long, uint64_t);
typedef int (*vfs_ready_fn)(struct vfs_node *);
typedef void (*vfs_open_fn)(struct vfs_node *);
typedef void (*vfs_close_fn)(struct vfs_node *);
typedef int64_t (*vfs_mmap_fn)(struct vfs_node *, struct file *, uint64_t,
                               uint64_t, uint64_t, uint64_t, uint64_t);

struct vfs_node {
    char name[128];
    uint32_t flags;
    uint32_t mode;
    uint32_t uid;
    uint32_t gid;
    uint32_t disk_inode;
    uint64_t inode;
    uint64_t length;
    uint64_t capacity;
    void *data;
    vfs_read_fn read;
    vfs_write_fn write;
    vfs_ioctl_fn ioctl;
    vfs_mmap_fn mmap;
    vfs_ready_fn read_ready;
    vfs_open_fn open;
    vfs_close_fn close;
    struct vfs_node *parent;
    struct vfs_node *children;
    struct vfs_node *next;
};

struct dirent {
    char name[128];
    uint64_t ino;
    uint32_t type;
};

/*
 * Persistence hooks: a filesystem driver can register these to observe
 * every mutation of the in-memory tree and mirror it to backing storage.
 * Handlers filter on node/parent state (e.g. disk_inode) themselves.
 */
struct vfs_persist_ops {
    void (*created)(struct vfs_node *node);
    void (*removed)(struct vfs_node *node);
    void (*moved)(struct vfs_node *node, struct vfs_node *old_parent,
                  const char *old_name);
    void (*written)(struct vfs_node *node, uint64_t offset, uint64_t size);
    void (*truncated)(struct vfs_node *node);
    void (*meta_changed)(struct vfs_node *node);
};

void vfs_set_persist_ops(const struct vfs_persist_ops *ops);
void vfs_notify_meta_changed(struct vfs_node *node);
void vfs_setup_memory_file(struct vfs_node *node);

extern struct vfs_node *vfs_root;

void vfs_init(void);
struct vfs_node *vfs_alloc_node(const char *name, uint32_t flags);
int vfs_attach(struct vfs_node *parent, struct vfs_node *child);
struct vfs_node *vfs_find_child(struct vfs_node *directory, const char *name);
struct vfs_node *vfs_lookup(const char *path);
struct vfs_node *vfs_lookup_nofollow(const char *path);
struct vfs_node *vfs_mkdir_p(const char *path);
struct vfs_node *vfs_create_file(const char *path, const void *data,
                                 uint64_t length, uint32_t flags, int copy_data);
struct vfs_node *vfs_create_file_node(const char *path, uint32_t mode);
struct vfs_node *vfs_create_directory(const char *path, uint32_t mode);
struct vfs_node *vfs_create_symlink(const char *path, const char *target,
                                    uint32_t flags);
int64_t vfs_readlink(struct vfs_node *node, void *buffer, size_t size);
int vfs_remove(const char *path, int remove_directory);
int vfs_rename(const char *old_path, const char *new_path);
int vfs_truncate(struct vfs_node *node, uint64_t length);
int64_t vfs_read(struct vfs_node *node, uint64_t offset, size_t size, void *buffer);
int64_t vfs_write(struct vfs_node *node, uint64_t offset, size_t size, const void *buffer);
int vfs_readdir(struct vfs_node *directory, uint64_t index, struct dirent *out);
int vfs_node_path(struct vfs_node *node, char *buffer, size_t capacity);

#endif
