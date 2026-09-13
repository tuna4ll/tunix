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
#define VFS_SOCKET      0x07U
#define VFS_READONLY    0x100U
#define VFS_OWNED_DATA  0x200U
#define VFS_INPUTDEVICE 0x400U
#define VFS_FRAMEBUFFER 0x800U
#define VFS_VOLATILE    0x1000U
#define VFS_ORPHANED    0x2000U
#define VFS_LAZY_DATA   0x4000U
#define VFS_HARDLINK    0x10000U
#define VFS_MOUNTPOINT  0x20000U
#define VFS_EVENTSTREAM 0x40000U

#define VFS_MS_RDONLY   0x0001U
#define VFS_MS_NOSUID   0x0002U
#define VFS_MS_NODEV    0x0004U
#define VFS_MS_NOEXEC   0x0008U
#define VFS_MS_REMOUNT  0x0020U
#define VFS_MS_BIND     0x1000U
#define VFS_MS_SUPPORTED (VFS_MS_RDONLY | VFS_MS_NOSUID | VFS_MS_NODEV | \
                          VFS_MS_NOEXEC | VFS_MS_REMOUNT | VFS_MS_BIND)
#define VFS_MS_SYNCHRONOUS 0x00000010U
#define VFS_MS_MANDLOCK    0x00000040U
#define VFS_MS_DIRSYNC     0x00000080U
#define VFS_MS_NOSYMFOLLOW 0x00000100U
#define VFS_MS_NOATIME     0x00000400U
#define VFS_MS_NODIRATIME  0x00000800U
#define VFS_MS_SILENT      0x00008000U
#define VFS_MS_POSIXACL    0x00010000U
#define VFS_MS_RELATIME    0x00200000U
#define VFS_MS_STRICTATIME 0x01000000U
#define VFS_MS_LAZYTIME    0x02000000U
#define VFS_MS_IGNORED (VFS_MS_SYNCHRONOUS | VFS_MS_MANDLOCK | VFS_MS_DIRSYNC | \
                        VFS_MS_NOSYMFOLLOW | VFS_MS_NOATIME | VFS_MS_NODIRATIME | \
                        VFS_MS_SILENT | VFS_MS_POSIXACL | VFS_MS_RELATIME | \
                        VFS_MS_STRICTATIME | VFS_MS_LAZYTIME)

struct vfs_node;
struct file;
struct pipe_buffer;

typedef int64_t (*vfs_read_fn)(struct vfs_node *, uint64_t, size_t, void *);
typedef int64_t (*vfs_write_fn)(struct vfs_node *, uint64_t, size_t, const void *);
typedef int64_t (*vfs_ioctl_fn)(struct vfs_node *, unsigned long, uint64_t);
typedef int64_t (*vfs_file_ioctl_fn)(struct file *, unsigned long, uint64_t);
typedef int (*vfs_ready_fn)(struct vfs_node *);
typedef void (*vfs_open_fn)(struct vfs_node *);
typedef void (*vfs_close_fn)(struct vfs_node *);
typedef int64_t (*vfs_mmap_fn)(struct vfs_node *, struct file *, uint64_t,
                               uint64_t, uint64_t, uint64_t, uint64_t);

struct vfs_page_map {
    uint64_t count;
    uint8_t **page;
    uint64_t *dirty;
    uint64_t resident;
};

struct vfs_node {
    char name[128];
    uint32_t flags;
    uint32_t mode;
    uint32_t uid;
    uint32_t gid;
    uint32_t disk_inode;
    uint32_t atime;
    uint32_t mtime;
    uint32_t ctime;
    uint64_t inode;
    uint64_t length;
    uint64_t capacity;
    uint32_t dev_major;
    uint32_t dev_minor;
    uint8_t stateless;
    void *data;
    struct vfs_page_map *pages;
    struct pipe_buffer *fifo;
    void *fs_private;
    vfs_read_fn read;
    vfs_write_fn write;
    int (*truncate)(struct vfs_node *node, uint64_t length);
    int (*adopt)(struct vfs_node *directory, struct vfs_node *child);
    vfs_ioctl_fn ioctl;
    vfs_file_ioctl_fn file_ioctl;
    vfs_mmap_fn mmap;
    vfs_ready_fn read_ready;
    vfs_ready_fn write_ready;
    vfs_open_fn open;
    vfs_close_fn close;
    void (*refresh)(struct vfs_node *directory);
    struct vfs_node *parent;
    struct vfs_node *children;
    struct vfs_node *next;
    struct vfs_node *link_target;
    uint32_t links;
    struct vfs_node *mounted;
    uint32_t refs;
    uint32_t mapped_refs;
    uint32_t shared_writers;
    uint64_t map_dirty_start;
    uint64_t map_dirty_end;
    struct file *flock_exclusive;
    uint32_t flock_shared;
    uint64_t posix_lock_pid;
    int posix_lock_write;
};

struct dirent {
    char name[128];
    uint64_t ino;
    uint32_t type;
};

struct vfs_mount {
    char source[64];
    char target[192];
    char type[16];
    uint32_t flags;
    struct vfs_node *mountpoint;
    struct vfs_node *root;
    int owns_root;
    struct vfs_mount *next;
};

int vfs_mount(const char *source, const char *target, const char *type,
              uint32_t flags);
int vfs_umount(const char *target);
void vfs_mount_builtin(const char *source, const char *target, const char *type,
                       struct vfs_node *root);
const struct vfs_mount *vfs_mounts(void);

struct vfs_persist_ops {
    void (*created)(struct vfs_node *node);
    void (*removed)(struct vfs_node *node);
    void (*moved)(struct vfs_node *node, struct vfs_node *old_parent,
                  const char *old_name);
    void (*written)(struct vfs_node *node, uint64_t offset, uint64_t size);
    void (*truncated)(struct vfs_node *node);
    void (*meta_changed)(struct vfs_node *node);
    void (*linked)(struct vfs_node *link);
    void (*released)(struct vfs_node *node);
    int (*fetch)(struct vfs_node *node);
    int (*fetch_page)(struct vfs_node *node, uint64_t index, void *out);
};

void vfs_set_persist_ops(const struct vfs_persist_ops *ops);
void vfs_notify_meta_changed(struct vfs_node *node);

int vfs_fault_in(struct vfs_node *node);
#define VFS_PAGE_SIZE 4096ULL

void *vfs_page(struct vfs_node *node, uint64_t index, int for_write);
void *vfs_page_peek(struct vfs_node *node, uint64_t index);
uint64_t vfs_page_physical(struct vfs_node *node, uint64_t index);
int vfs_page_is_dirty(struct vfs_node *node, uint64_t index);
void vfs_page_clear_dirty(struct vfs_node *node, uint64_t index);
uint64_t vfs_page_span(struct vfs_node *node);
void vfs_release_data(struct vfs_node *node);
uint64_t vfs_drop_clean_pages(struct vfs_node *node);

void vfs_node_ref(struct vfs_node *node);
void vfs_node_unref(struct vfs_node *node);

void vfs_map_ref(struct vfs_node *node);
void vfs_map_unref(struct vfs_node *node);

void vfs_map_write_ref(struct vfs_node *node, uint64_t offset, uint64_t length);
void vfs_map_write_unref(struct vfs_node *node);
void vfs_flush_mapped(struct vfs_node *node);

#define VFS_TIME_ATIME 0x1U
#define VFS_TIME_MTIME 0x2U
#define VFS_TIME_CTIME 0x4U
void vfs_stamp_times(struct vfs_node *node, uint32_t which);
void vfs_setup_memory_file(struct vfs_node *node);

extern struct vfs_node *vfs_root;

uint64_t vfs_reclaim_file_data(struct vfs_node *node);
uint64_t vfs_cached_bytes(void);
void vfs_trim_cache(uint64_t budget);

void vfs_init(void);
struct vfs_node *vfs_alloc_node(const char *name, uint32_t flags);
int vfs_attach(struct vfs_node *parent, struct vfs_node *child);
struct vfs_node *vfs_find_child(struct vfs_node *directory, const char *name);
struct vfs_node *vfs_find_entry(struct vfs_node *directory, const char *name);
int vfs_link(struct vfs_node *target, const char *path);
struct vfs_node *vfs_attach_link(struct vfs_node *parent, const char *name,
                                 struct vfs_node *target);
struct vfs_node *vfs_lookup(const char *path);
struct vfs_node *vfs_lookup_nofollow(const char *path);
struct vfs_node *vfs_mkdir_p(const char *path);
struct vfs_node *vfs_create_file(const char *path, const void *data,
                                 uint64_t length, uint32_t flags, int copy_data);
struct vfs_node *vfs_create_file_node(const char *path, uint32_t mode);
struct vfs_node *vfs_create_directory(const char *path, uint32_t mode);
struct vfs_node *vfs_create_fifo(const char *path, uint32_t mode);
struct vfs_node *vfs_create_socket_node(const char *path, uint32_t mode);
struct vfs_node *vfs_create_symlink(const char *path, const char *target,
                                    uint32_t flags);
struct vfs_node *vfs_attach_symlink(struct vfs_node *parent, const char *name,
                                    const char *target);
int vfs_detach_child(struct vfs_node *parent, struct vfs_node *node);
int64_t vfs_readlink(struct vfs_node *node, void *buffer, size_t size);
int vfs_remove(const char *path, int remove_directory);
int vfs_rename(const char *old_path, const char *new_path);
int vfs_truncate(struct vfs_node *node, uint64_t length);
int64_t vfs_read(struct vfs_node *node, uint64_t offset, size_t size, void *buffer);
int64_t vfs_write(struct vfs_node *node, uint64_t offset, size_t size, const void *buffer);
int vfs_readdir(struct vfs_node *directory, uint64_t index, struct dirent *out);
int vfs_node_path(struct vfs_node *node, char *buffer, size_t capacity);

#endif
