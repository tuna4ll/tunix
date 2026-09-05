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
/* The name a bound AF_UNIX socket has in the filesystem, which holds nothing itself. */
#define VFS_SOCKET      0x07U
#define VFS_READONLY    0x100U
#define VFS_OWNED_DATA  0x200U
#define VFS_INPUTDEVICE 0x400U
#define VFS_FRAMEBUFFER 0x800U
#define VFS_VOLATILE    0x1000U
/* Detached from the tree but still referenced -- see vfs_node_unref(). */
#define VFS_ORPHANED    0x2000U
/* Contents still on disk: data is NULL, length is set. See vfs_fault_in(). */
#define VFS_LAZY_DATA   0x4000U
/* An ioctl that carries the descriptor, for a device whose objects belong
   to one open file. */
#define VFS_HARDLINK    0x10000U
/* A filesystem is mounted over this directory; `mounted` is its root. */
#define VFS_MOUNTPOINT  0x20000U

/* Mount flags, with Linux's numbers because userspace passes Linux's. */
#define VFS_MS_RDONLY   0x0001U
#define VFS_MS_NOSUID   0x0002U
#define VFS_MS_NODEV    0x0004U
#define VFS_MS_NOEXEC   0x0008U
#define VFS_MS_REMOUNT  0x0020U
#define VFS_MS_BIND     0x1000U
#define VFS_MS_SUPPORTED (VFS_MS_RDONLY | VFS_MS_NOSUID | VFS_MS_NODEV | \
                          VFS_MS_NOEXEC | VFS_MS_REMOUNT | VFS_MS_BIND)
/* Flags that say how a filesystem behaves rather than what is being mounted. */
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
/* The same, for a device whose objects belong to one open file rather than to
   the node they were reached through. */
typedef int64_t (*vfs_file_ioctl_fn)(struct file *, unsigned long, uint64_t);
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
    /* Epoch seconds, matching the width ext2 stores on disk. The format has no
       sub-second field, so stat reports a nanosecond part of zero. */
    uint32_t atime;
    uint32_t mtime;
    uint32_t ctime;
    uint64_t inode;
    uint64_t length;
    uint64_t capacity;
    /* Device number for character and block devices, as st_rdev reports it. */
    uint32_t dev_major;
    uint32_t dev_minor;
    void *data;
    /* The buffer behind a FIFO. Created on the first open and freed with the
       node, so that a FIFO nobody has open is still a FIFO. */
    struct pipe_buffer *fifo;
    /* Whatever the owning filesystem needs to find this node again on the medium. */
    void *fs_private;
    vfs_read_fn read;
    vfs_write_fn write;
    /* Set by a filesystem that stores contents itself, so a size change
       reaches the medium. */
    int (*truncate)(struct vfs_node *node, uint64_t length);
    /* Set on a directory whose filesystem has to make a new child real on
       the medium. */
    int (*adopt)(struct vfs_node *directory, struct vfs_node *child);
    vfs_ioctl_fn ioctl;
    /* Preferred over `ioctl` when set, because it can tell one client from
       another. */
    vfs_file_ioctl_fn file_ioctl;
    vfs_mmap_fn mmap;
    vfs_ready_fn read_ready;
    /* Whether a write can make progress, which matters for a device with a
       fixed-size ring. */
    vfs_ready_fn write_ready;
    vfs_open_fn open;
    vfs_close_fn close;
    /* Rebuild a directory whose contents are computed rather than stored, run
       before it is searched or read. /proc/<pid>/fd is the reason it exists. */
    void (*refresh)(struct vfs_node *directory);
    struct vfs_node *parent;
    struct vfs_node *children;
    struct vfs_node *next;
    /* Set on a hard link, naming the node that holds the contents. */
    struct vfs_node *link_target;
    /* Names this node answers to. One for everything that is not linked. */
    uint32_t links;
    /* The root of the filesystem mounted over this directory, if any. Every
       path walk that reaches this node continues there instead. */
    struct vfs_node *mounted;
    /* Holders outside the directory tree, such as a process's working directory. */
    uint32_t refs;
    /* Mappings that point straight at these pages rather than at a copy of them. */
    uint32_t mapped_refs;
    /* Advisory whole-file locks, held by the open file description. */
    struct file *flock_exclusive;
    uint32_t flock_shared;
    /* POSIX advisory locks, which are a separate space from flock and owned
       by the process. */
    uint64_t posix_lock_pid;
    int posix_lock_write;
};

struct dirent {
    char name[128];
    uint64_t ino;
    uint32_t type;
};

/* One line of /proc/mounts, with no mountpoint for the trees the system boots with. */
struct vfs_mount {
    char source[64];
    char target[192];
    char type[16];
    uint32_t flags;
    struct vfs_node *mountpoint;
    struct vfs_node *root;
    int owns_root;                 /* the tree is ours to free on umount */
    struct vfs_mount *next;
};

/* 0 on success, a negative errno on failure. */
int vfs_mount(const char *source, const char *target, const char *type,
              uint32_t flags);
int vfs_umount(const char *target);
/* Declare a tree the system came up with, so it is reported like a mount. */
void vfs_mount_builtin(const char *source, const char *target, const char *type,
                       struct vfs_node *root);
const struct vfs_mount *vfs_mounts(void);

/* Persistence hooks, for a driver that mirrors every mutation of the
   tree to a medium. */
struct vfs_persist_ops {
    void (*created)(struct vfs_node *node);
    void (*removed)(struct vfs_node *node);
    void (*moved)(struct vfs_node *node, struct vfs_node *old_parent,
                  const char *old_name);
    void (*written)(struct vfs_node *node, uint64_t offset, uint64_t size);
    void (*truncated)(struct vfs_node *node);
    void (*meta_changed)(struct vfs_node *node);
    /* A hard link was made; the node is the new name, not the contents. */
    void (*linked)(struct vfs_node *link);
    /* The last name of a node that outlived its own directory entry is gone. */
    void (*released)(struct vfs_node *node);
    /* Fill in a VFS_LAZY_DATA node's contents. 0 on success. */
    int (*fetch)(struct vfs_node *node);
};

void vfs_set_persist_ops(const struct vfs_persist_ops *ops);
void vfs_notify_meta_changed(struct vfs_node *node);

/* Make node->data usable. Every path that dereferences it must call this. */
int vfs_fault_in(struct vfs_node *node);
/* Move a file's cached contents onto a page boundary so mmap can map them. */
int vfs_align_data(struct vfs_node *node);
/* Drop cached contents that can be fetched again, once a caller has taken its
   own copy. Nothing may hold a pointer into node->data across this. */
void vfs_release_data(struct vfs_node *node);

/* Hold a node that lives outside the directory tree, such as a process's cwd.
   Unreferencing an orphaned node is what finally frees it. */
void vfs_node_ref(struct vfs_node *node);
void vfs_node_unref(struct vfs_node *node);

/* Claim the node's cached contents for a mapping, and give them back when it goes. */
void vfs_map_ref(struct vfs_node *node);
void vfs_map_unref(struct vfs_node *node);

#define VFS_TIME_ATIME 0x1U
#define VFS_TIME_MTIME 0x2U
#define VFS_TIME_CTIME 0x4U
/* Stamp the selected timestamps with the current time. */
void vfs_stamp_times(struct vfs_node *node, uint32_t which);
void vfs_setup_memory_file(struct vfs_node *node);

extern struct vfs_node *vfs_root;

/* Release the cached contents of every file under `node` that can be read back
   off the disk, returning the bytes handed back to the heap. */
uint64_t vfs_reclaim_file_data(struct vfs_node *node);
/* File contents currently held in the heap and replaceable from the disk. */
uint64_t vfs_cached_bytes(void);
/* Drop least-recently-used contents until the cache is inside `budget`. */
void vfs_trim_cache(uint64_t budget);
/* Drop every pointer into memory the VFS does not own, for files the disk can
   now answer for. Returns how many files were cut loose. */

void vfs_init(void);
struct vfs_node *vfs_alloc_node(const char *name, uint32_t flags);
int vfs_attach(struct vfs_node *parent, struct vfs_node *child);
struct vfs_node *vfs_find_child(struct vfs_node *directory, const char *name);
/* The directory entry itself, hard links left unresolved. Only the code that
   has to act on the name rather than on the contents wants this. */
struct vfs_node *vfs_find_entry(struct vfs_node *directory, const char *name);
/* Give `target` a further name. Directories are refused. */
int vfs_link(struct vfs_node *target, const char *path);
/* Attach a name for an existing node directly, for a tree being rebuilt. */
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
/* Attach directly to a parent node, for trees that are built rather than
   named -- a refresh handler cannot look its own directory up by path. */
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
