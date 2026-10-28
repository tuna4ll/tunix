#include "ext4.h"

#include <tunix/acl.h>
#include <tunix/heap.h>
#include <tunix/kstring.h>
#include <tunix/xattr.h>

static uint8_t data_buf[EXT4_MAX_BLOCK_SIZE] __attribute__((aligned(16)));
static uint8_t io_buf[EXT4_RUN_BYTES] __attribute__((aligned(16)));
static uint8_t run_buf[EXT4_RUN_BYTES] __attribute__((aligned(16)));

static void adopt(struct vfs_node *node, uint32_t ino) {
    node->disk_inode = ino;
    node->fs_private = fs;
}

static void forget(struct vfs_node *node) {
    vfs_forget_backing(node);
    node->fs_private = NULL;
}

void ext4_restore_owner(struct vfs_node *node, const struct ext4_inode *inode) {
    node->mode = inode->i_mode & 07777U;
    node->uid = inode_uid(inode);
    node->gid = inode_gid(inode);
    inode_get_times(inode, node);
}

static uint16_t mode_type_for(const struct vfs_node *node) {
    switch (node->flags & 0xFFU) {
    case VFS_DIRECTORY:   return EXT4_S_IFDIR;
    case VFS_SYMLINK:     return EXT4_S_IFLNK;
    case VFS_PIPE:        return EXT4_S_IFIFO;
    case VFS_SOCKET:      return EXT4_S_IFSOCK;
    case VFS_CHARDEVICE:  return EXT4_S_IFCHR;
    case VFS_BLOCKDEVICE: return EXT4_S_IFBLK;
    default:              return EXT4_S_IFREG;
    }
}

static uint8_t dirent_type_for(const struct vfs_node *node) {
    switch (node->flags & 0xFFU) {
    case VFS_DIRECTORY:   return EXT4_FT_DIR;
    case VFS_SYMLINK:     return EXT4_FT_SYMLINK;
    case VFS_PIPE:        return EXT4_FT_FIFO;
    case VFS_SOCKET:      return EXT4_FT_SOCK;
    case VFS_CHARDEVICE:  return EXT4_FT_CHRDEV;
    case VFS_BLOCKDEVICE: return EXT4_FT_BLKDEV;
    default:              return EXT4_FT_REG_FILE;
    }
}

static int persistable(uint32_t kind) {
    return kind == VFS_FILE || kind == VFS_DIRECTORY || kind == VFS_SYMLINK || kind == VFS_PIPE ||
        kind == VFS_SOCKET || kind == VFS_CHARDEVICE || kind == VFS_BLOCKDEVICE;
}

static void encode_device(struct ext4_inode *inode, uint32_t major, uint32_t minor) {
    if (major < 256U && minor < 256U) {
        inode->i_block[0] = (major << 8) | minor;
        inode->i_block[1] = 0;
    } else {
        inode->i_block[0] = 0;
        inode->i_block[1] = (minor & 0xFFU) | (major << 8) | ((minor & ~0xFFU) << 12);
    }
}

static void decode_device(const struct ext4_inode *inode, uint32_t *major, uint32_t *minor) {
    if (inode->i_block[0]) {
        *major = (inode->i_block[0] >> 8) & 0xFFU;
        *minor = inode->i_block[0] & 0xFFU;
    } else {
        uint32_t value = inode->i_block[1];
        *major = (value & 0xFFF00U) >> 8;
        *minor = (value & 0xFFU) | ((value >> 12) & 0xFFF00U);
    }
}

static int under_volatile(const struct vfs_node *node) {
    for (const struct vfs_node *walk = node->parent; walk && walk != fs->root; walk = walk->parent)
        if (walk->flags & VFS_VOLATILE) return 1;
    return 0;
}

struct run_writer {
    uint64_t start_block;
    uint32_t count;
};

static int run_flush(struct run_writer *run) {
    if (!run->count) return 0;
    int status = ext4_volume_write(fs, run->start_block, run->count, run_buf);
    run->count = 0;
    return status;
}

static int run_append(struct run_writer *run, uint64_t block, const void *data) {
    uint32_t capacity = EXT4_RUN_BYTES / fs->block_size;
    if (run->count && (block != run->start_block + run->count || run->count == capacity)) {
        if (run_flush(run) != 0) return -1;
    }
    if (!run->count) run->start_block = block;
    memcpy(run_buf + (size_t)run->count * fs->block_size, data, fs->block_size);
    run->count++;
    return 0;
}

static int write_page(uint32_t ino, struct ext4_inode *inode, uint64_t length, uint64_t index,
                      const uint8_t *page, struct run_writer *run, int *allocated) {
    uint32_t block_size = fs->block_size;
    for (uint32_t part = 0; part < VFS_PAGE_SIZE / block_size; part++) {
        uint64_t start = index * VFS_PAGE_SIZE + (uint64_t)part * block_size;
        if (start >= length) break;
        ext4_failed_stage = "block map";
        int dirty = 0;
        int64_t block = inode_bmap(ino, inode, start / block_size, 1, &dirty);
        if (block <= 0) return -1;
        if (dirty) *allocated = 1;
        const uint8_t *source = page + (size_t)part * block_size;
        uint64_t available = length - start;
        if (available < block_size) {
            memset(data_buf, 0, block_size);
            memcpy(data_buf, source, (size_t)available);
            source = data_buf;
        }
        ext4_failed_stage = "data write";
        if (run_append(run, (uint64_t)block, source) != 0) return -1;
    }
    return 0;
}

static void stamp_from_node(struct ext4_inode *inode, const struct vfs_node *node) {
    inode_set_times(inode, node->atime, node->atime_ns, node->mtime, node->mtime_ns, node->ctime,
                    node->ctime_ns);
}

static int file_write_range(uint32_t ino, struct vfs_node *node, uint64_t offset, uint64_t size) {
    struct ext4_inode inode;
    ext4_failed_stage = "inode read";
    if (inode_read(ino, &inode) != 0) return -1;
    int allocated = 0;
    uint64_t end = offset + size;
    if (end > node->length) end = node->length;
    if (size && end > offset) {
        uint64_t first = offset / VFS_PAGE_SIZE;
        uint64_t last = (end - 1U) / VFS_PAGE_SIZE;
        struct run_writer run = {0, 0};
        for (uint64_t index = first; index <= last; index++) {
            const uint8_t *page = (const uint8_t *)vfs_page_peek(node, index);
            if (!page || !vfs_page_is_dirty(node, index)) continue;
            if (write_page(ino, &inode, node->length, index, page, &run, &allocated) != 0) {
                run_flush(&run);
                inode_write(ino, &inode);
                return -1;
            }
            vfs_page_clear_dirty(node, index);
        }
        ext4_failed_stage = "data write";
        if (run_flush(&run) != 0) return -1;
    }
    inode_set_size(&inode, node->length);
    stamp_from_node(&inode, node);
    ext4_failed_stage = "inode write";
    if (inode_write(ino, &inode) != 0) return -1;
    return allocated;
}

static int store_node_xattrs(uint32_t ino, struct ext4_inode *inode, struct vfs_node *node) {
    struct ext4_xattr_set set;
    if (xattr_load(ino, inode, &set) != 0) memset(&set, 0, sizeof(set));
    int status = xattr_from_vfs(node, &set);
    if (status == 0) status = xattr_store(ino, inode, &set);
    xattr_set_free(&set);
    node->xattr_dirty = 0;
    return status;
}

static int create_one(struct vfs_node *node) {
    if (!node->parent || ext4_volume_of(node->parent) != fs || node->disk_inode) return -1;
    uint32_t kind = node->flags & 0xFFU;
    if (node->flags & VFS_VOLATILE) return 1;
    if (under_volatile(node)) return 1;
    if (node->link_target) return 1;
    if (!persistable(kind)) return 1;

    ext4_failed_stage = "inode allocation";
    uint32_t parent_ino = node->parent->disk_inode;
    uint32_t ino = alloc_inode(parent_ino, kind == VFS_DIRECTORY);
    if (!ino) return -1;

    struct ext4_inode inode;
    memset(&inode, 0, sizeof(inode));
    if (fs->inode_size > EXT4_GOOD_OLD_INODE_SIZE) inode.i_extra_isize = fs->extra_isize;
    inode.i_mode = (uint16_t)(mode_type_for(node) | (node->mode & 07777U));
    inode_set_owner(&inode, node->uid, node->gid);
    stamp_from_node(&inode, node);
    uint32_t seconds;
    uint32_t nanoseconds;
    ext4_now_ns(&seconds, &nanoseconds);
    inode_set_crtime(&inode, seconds, nanoseconds);
    inode.i_links_count = 1;
    int extents = has_incompat(EXT4_INCOMPAT_EXTENTS);
    if (extents && (kind == VFS_FILE || kind == VFS_DIRECTORY)) ext_init_root(&inode);
    if (inode_write_new(ino, &inode) != 0) {
        free_inode(ino, kind == VFS_DIRECTORY);
        return -1;
    }

    if (kind == VFS_DIRECTORY) {
        ext4_failed_stage = "directory block";
        if (dir_create_block(ino, &inode, parent_ino) != 0) {
            inode_release_blocks(ino, &inode);
            free_inode(ino, 1);
            return -1;
        }
        inode.i_links_count = 2;
    } else if (kind == VFS_SYMLINK) {
        uint64_t length = node->length;
        inode_set_size(&inode, length);
        if (length < EXT4_N_BLOCKS * 4U) {
            memcpy(inode.i_block, node->data, (size_t)length);
        } else if (length < fs->block_size) {
            ext4_failed_stage = "symlink block";
            if (extents) ext_init_root(&inode);
            int dirty = 0;
            int64_t block = inode_bmap(ino, &inode, 0, 1, &dirty);
            memset(data_buf, 0, fs->block_size);
            memcpy(data_buf, node->data, (size_t)length);
            if (block <= 0 || buf_write((uint64_t)block, data_buf) != 0) {
                inode_release_blocks(ino, &inode);
                free_inode(ino, 0);
                return -1;
            }
        } else {
            free_inode(ino, 0);
            return -1;
        }
    } else if (kind == VFS_CHARDEVICE || kind == VFS_BLOCKDEVICE) {
        encode_device(&inode, node->dev_major, node->dev_minor);
    }

    ext4_failed_stage = "inode write";
    if (inode_write(ino, &inode) != 0) {
        inode_release_blocks(ino, &inode);
        free_inode(ino, kind == VFS_DIRECTORY);
        return -1;
    }
    ext4_failed_stage = "directory entry";
    if (dir_add_entry(parent_ino, node->name, ino, dirent_type_for(node)) != 0) {
        inode_release_blocks(ino, &inode);
        inode.i_links_count = 0;
        inode.i_dtime = ext4_now();
        inode_write(ino, &inode);
        free_inode(ino, kind == VFS_DIRECTORY);
        return -1;
    }
    if (kind == VFS_DIRECTORY) inode_links_adjust(parent_ino, 1);
    adopt(node, ino);
    if (node->acl || vfs_xattr_first(node) || node->xattr_dirty) {
        if (inode_read(ino, &inode) == 0) store_node_xattrs(ino, &inode, node);
    }
    if (kind == VFS_FILE && node->length) file_write_range(ino, node, 0, node->length);
    return 0;
}

static int link_one(struct vfs_node *link) {
    struct vfs_node *target = link->link_target;
    if (!link->parent || ext4_volume_of(link->parent) != fs || !target ||
        ext4_volume_of(target) != fs)
        return -1;
    if (dir_add_entry(link->parent->disk_inode, link->name, target->disk_inode,
                      dirent_type_for(target)) != 0)
        return -1;
    inode_links_adjust(target->disk_inode, 1);
    return 0;
}

static int unlink_one(struct vfs_node *body, uint32_t parent_ino, const char *name) {
    if (!body || !body->disk_inode) return -1;
    dir_remove_entry(parent_ino, name);
    inode_links_adjust(body->disk_inode, -1);
    return 0;
}

static void release_inode(uint32_t ino) {
    struct ext4_inode inode;
    if (inode_read(ino, &inode) != 0) return;
    int is_directory = inode_kind(&inode) == EXT4_S_IFDIR;
    if (!inode_is_fast_symlink(&inode) && !(inode.i_flags & EXT4_INLINE_DATA_FL))
        inode_release_blocks(ino, &inode);
    xattr_release(ino, &inode);
    if (inode.i_flags & EXT4_INLINE_DATA_FL) memset(inode.i_block, 0, sizeof(inode.i_block));
    inode.i_links_count = 0;
    inode_set_size(&inode, 0);
    inode.i_dtime = ext4_now();
    inode_write(ino, &inode);
    free_inode(ino, is_directory);
}

static int remove_one(struct vfs_node *node, uint32_t parent_ino, const char *name) {
    uint32_t ino = node->disk_inode;
    if (!ino) return -1;
    dir_remove_entry(parent_ino, name);
    struct ext4_inode inode;
    if (inode_read(ino, &inode) != 0) return -1;
    if (inode_kind(&inode) == EXT4_S_IFDIR) inode_links_adjust(parent_ino, -1);
    release_inode(ino);
    forget(node);
    return 0;
}

static int seed_errors;

static struct vfs_node *next_in_subtree(struct vfs_node *node, struct vfs_node *top) {
    while (node != top && !node->next) node = node->parent;
    return node == top ? NULL : node->next;
}

static void persist_subtree(struct vfs_node *top) {
    struct vfs_node *node = top;
    while (node) {
        int status = create_one(node);
        if (status < 0) seed_errors++;
        if (status == 0 && (node->flags & 0xFFU) == VFS_DIRECTORY && node->children) {
            node = node->children;
            continue;
        }
        node = next_in_subtree(node, top);
    }
}

static void persist_links(struct vfs_node *top) {
    struct vfs_node *node = top->children;
    while (node) {
        if (node->link_target) {
            if (link_one(node) != 0) seed_errors++;
        } else if ((node->flags & 0xFFU) == VFS_DIRECTORY && !(node->flags & VFS_VOLATILE) &&
                   node->children) {
            node = node->children;
            continue;
        }
        node = next_in_subtree(node, top);
    }
}

static int unpersist_descends(const struct vfs_node *node) {
    return !node->link_target && ext4_volume_of(node) == fs && node->links <= 1 &&
        (node->flags & 0xFFU) == VFS_DIRECTORY && node->children;
}

static void unpersist_one(struct vfs_node *node, uint32_t parent_ino, const char *name) {
    if (node->link_target) {
        if (ext4_volume_of(node->link_target) == fs)
            unlink_one(node->link_target, parent_ino, name);
        return;
    }
    if (ext4_volume_of(node) != fs) return;
    if (node->links > 1) {
        unlink_one(node, parent_ino, name);
        return;
    }
    if ((node->flags & 0xFFU) == VFS_FILE && vfs_fault_in(node) != 0) {
        ext4_failed_stage = "data read";
        ext4_report_failure("move", node->name);
    }
    remove_one(node, parent_ino, name);
}

static void unpersist_subtree(struct vfs_node *top, uint32_t parent_ino, const char *name) {
    struct vfs_node *node = top;
    while (unpersist_descends(node)) node = node->children;
    for (;;) {
        if (node == top) {
            unpersist_one(node, parent_ino, name);
            return;
        }
        struct vfs_node *parent = node->parent;
        struct vfs_node *next = node->next;
        unpersist_one(node, parent->disk_inode, node->name);
        if (next) {
            node = next;
            while (unpersist_descends(node)) node = node->children;
        } else {
            node = parent;
        }
    }
}

static void event_created(struct vfs_node *node) {
    struct ext4_volume *volume = node ? ext4_active_volume_of(node->parent) : NULL;
    if (!volume) return;
    struct ext4_volume *saved = ext4_op_enter(volume);
    int status = create_one(node);
    if (status < 0) ext4_report_failure("create", node->name);
    else if (status == 0) ext4_flush_meta();
    ext4_op_leave(saved);
}

static void orphan_if_unlinked(uint32_t ino) {
    struct ext4_inode inode;
    if (inode_read(ino, &inode) == 0 && !inode.i_links_count) orphan_add(ino);
}

static void event_removed(struct vfs_node *node) {
    struct ext4_volume *volume = node ? ext4_active_volume_of(node->parent) : NULL;
    if (!volume) return;
    struct ext4_volume *saved = ext4_op_enter(volume);
    uint32_t parent_ino = node->parent->disk_inode;
    if (node->link_target) {
        if (ext4_volume_of(node->link_target) == volume) {
            unlink_one(node->link_target, parent_ino, node->name);
            orphan_if_unlinked(node->link_target->disk_inode);
            ext4_flush_meta();
        }
    } else if (ext4_volume_of(node) == volume) {
        if (node->links > 1 || ((node->flags & 0xFFU) == VFS_FILE && node->refs)) {
            unlink_one(node, parent_ino, node->name);
            orphan_if_unlinked(node->disk_inode);
        } else {
            remove_one(node, parent_ino, node->name);
        }
        ext4_flush_meta();
    }
    ext4_op_leave(saved);
}

static void event_linked(struct vfs_node *link) {
    struct ext4_volume *volume = link ? ext4_active_volume_of(link->parent) : NULL;
    if (!volume || ext4_volume_of(link->link_target) != volume) return;
    struct ext4_volume *saved = ext4_op_enter(volume);
    if (link_one(link) != 0) kprintf("EXT4: cannot link %s\n", link->name);
    else ext4_flush_meta();
    ext4_op_leave(saved);
}

static void event_released(struct vfs_node *node) {
    struct ext4_volume *volume = ext4_active_volume_of(node);
    if (!volume) return;
    struct ext4_volume *saved = ext4_op_enter(volume);
    struct ext4_inode inode;
    if (inode_read(node->disk_inode, &inode) == 0 && !inode.i_links_count) {
        orphan_remove(node->disk_inode);
        release_inode(node->disk_inode);
    }
    forget(node);
    ext4_flush_meta();
    ext4_op_leave(saved);
}

static void move_link(struct vfs_node *node, struct vfs_node *old_parent, const char *old_name) {
    struct vfs_node *target = node->link_target;
    struct ext4_volume *volume = ext4_active_volume_of(target);
    if (!volume) return;
    int was = ext4_volume_of(old_parent) == volume;
    int now = ext4_volume_of(node->parent) == volume;
    if (!was && !now) return;
    struct ext4_volume *saved = ext4_op_enter(volume);
    if (was) dir_remove_entry(old_parent->disk_inode, old_name);
    if (now)
        dir_add_entry(node->parent->disk_inode, node->name, target->disk_inode,
                      dirent_type_for(target));
    if (was != now) inode_links_adjust(target->disk_inode, now ? 1 : -1);
    ext4_flush_meta();
    ext4_op_leave(saved);
}

static void event_moved(struct vfs_node *node, struct vfs_node *old_parent, const char *old_name) {
    if (!node) return;
    if (node->link_target) {
        move_link(node, old_parent, old_name);
        return;
    }
    struct ext4_volume *home = ext4_volume_of(node);
    struct ext4_volume *from = ext4_volume_of(old_parent);
    struct ext4_volume *to = ext4_volume_of(node->parent);
    if ((home && (home->loading || home->read_only)) || (to && (to->loading || to->read_only)))
        return;

    if (home && home == from && home == to) {
        struct ext4_volume *saved = ext4_op_enter(home);
        uint32_t old_parent_ino = old_parent->disk_inode;
        uint32_t new_parent_ino = node->parent->disk_inode;
        dir_remove_entry(old_parent_ino, old_name);
        dir_add_entry(new_parent_ino, node->name, node->disk_inode, dirent_type_for(node));
        if ((node->flags & 0xFFU) == VFS_DIRECTORY && old_parent_ino != new_parent_ino) {
            dir_set_dotdot(node->disk_inode, new_parent_ino);
            inode_links_adjust(old_parent_ino, -1);
            inode_links_adjust(new_parent_ino, 1);
        }
        inode_links_adjust(node->disk_inode, 0);
        ext4_flush_meta();
        ext4_op_leave(saved);
        return;
    }
    if (home && home == from) {
        struct ext4_volume *saved = ext4_op_enter(home);
        unpersist_subtree(node, old_parent->disk_inode, old_name);
        ext4_flush_meta();
        ext4_op_leave(saved);
    }
    if (to && !node->disk_inode) {
        struct ext4_volume *saved = ext4_op_enter(to);
        persist_subtree(node);
        if ((node->flags & 0xFFU) == VFS_DIRECTORY) persist_links(node);
        ext4_flush_meta();
        ext4_op_leave(saved);
    }
}

static void event_truncated(struct vfs_node *node) {
    struct ext4_volume *volume = ext4_active_volume_of(node);
    if (!volume || (node->flags & 0xFFU) != VFS_FILE) return;
    struct ext4_volume *saved = ext4_op_enter(volume);
    struct ext4_inode inode;
    uint32_t ino = node->disk_inode;
    if (inode_read(ino, &inode) == 0) {
        if (inode.i_flags & EXT4_INLINE_DATA_FL) inline_expand(ino, &inode);
        uint64_t keep = (node->length + fs->block_size - 1U) / fs->block_size;
        inode_truncate_blocks(ino, &inode, keep);
        inode_set_size(&inode, node->length);
        stamp_from_node(&inode, node);
        if (inode_write(ino, &inode) == 0) ext4_flush_meta();
    }
    node->io_generation++;
    ext4_op_leave(saved);
}

static void event_meta_changed(struct vfs_node *node) {
    struct ext4_volume *volume = ext4_active_volume_of(node);
    if (!volume) return;
    struct ext4_volume *saved = ext4_op_enter(volume);
    struct ext4_inode inode;
    uint32_t ino = node->disk_inode;
    if (inode_read(ino, &inode) == 0) {
        inode.i_mode = (uint16_t)((inode.i_mode & EXT4_S_IFMT) | (node->mode & 07777U));
        inode_set_owner(&inode, node->uid, node->gid);
        stamp_from_node(&inode, node);
        int status =
            node->xattr_dirty ? store_node_xattrs(ino, &inode, node) : inode_write(ino, &inode);
        if (status == 0) ext4_flush_meta();
        else ext4_report_failure("store attributes of", node->name);
    }
    ext4_op_leave(saved);
}

struct data_extent {
    uint64_t block;
    uint32_t bytes;
    const uint8_t *source;
};

static int map_page(uint32_t ino, struct ext4_inode *inode, uint64_t length, uint64_t index,
                    const uint8_t *page, struct data_extent *out, uint32_t *used, int *allocated) {
    uint32_t block_size = fs->block_size;
    for (uint32_t part = 0; part < VFS_PAGE_SIZE / block_size; part++) {
        uint64_t start = index * VFS_PAGE_SIZE + (uint64_t)part * block_size;
        if (start >= length) break;
        ext4_failed_stage = "block map";
        int dirty = 0;
        int64_t block = inode_bmap(ino, inode, start / block_size, 1, &dirty);
        if (block <= 0) return -1;
        if (dirty) *allocated = 1;
        uint64_t available = length - start;
        out[*used].block = (uint64_t)block;
        out[*used].bytes = available < block_size ? (uint32_t)available : block_size;
        out[*used].source = page + (size_t)part * block_size;
        (*used)++;
    }
    return 0;
}

static int write_extents(struct ext4_volume *volume, const struct data_extent *extents,
                         uint32_t count) {
    uint32_t block_size = volume->block_size;
    uint32_t capacity = EXT4_RUN_BYTES / block_size;
    for (uint32_t index = 0; index < count;) {
        uint32_t run = 0;
        while (index + run < count && run < capacity &&
               extents[index + run].block == extents[index].block + run) {
            const struct data_extent *extent = &extents[index + run];
            uint8_t *slot = io_buf + (size_t)run * block_size;
            memcpy(slot, extent->source, extent->bytes);
            if (extent->bytes < block_size)
                memset(slot + extent->bytes, 0, block_size - extent->bytes);
            run++;
        }
        if (ext4_volume_write(volume, extents[index].block, run, io_buf) != 0) return -1;
        index += run;
    }
    return 0;
}

static int writeback(struct vfs_node *node, const struct vfs_writeback *batch) {
    mutex_lock(&ext4_io_lock);
    mutex_lock(&ext4_lock);
    struct ext4_volume *volume = ext4_active_volume_of(node);
    if (!volume || node->disk_inode != batch->ino || node->io_generation != batch->generation) {
        mutex_unlock(&ext4_lock);
        mutex_unlock(&ext4_io_lock);
        return VFS_WRITEBACK_STALE;
    }
    struct ext4_volume *saved = ext4_enter(volume);
    uint32_t per_page = VFS_PAGE_SIZE / fs->block_size;
    struct data_extent *extents =
        (struct data_extent *)kmalloc((size_t)batch->count * per_page * sizeof(*extents));
    uint32_t used = 0;
    int status = extents ? -1 : -ENOMEM;
    int allocated = 0;
    struct ext4_inode inode;
    ext4_failed_stage = "inode read";
    if (extents && inode_read(batch->ino, &inode) == 0) {
        status = 0;
        if (inode.i_flags & EXT4_INLINE_DATA_FL) {
            status = inline_expand(batch->ino, &inode);
            allocated = 1;
        }
        for (uint32_t index = 0; index < batch->count && status == 0; index++)
            status = map_page(batch->ino, &inode, batch->length, batch->indices[index],
                              batch->pages[index], extents, &used, &allocated);
        if (batch->length > inode_size_of(&inode)) inode_set_size(&inode, batch->length);
        inode_set_times(&inode, batch->atime, batch->atime_ns, batch->mtime, batch->mtime_ns,
                        batch->ctime, batch->ctime_ns);
        ext4_failed_stage = "inode write";
        if (inode_write(batch->ino, &inode) != 0) status = -1;
    }
    if (allocated) ext4_flush_meta();
    if (status != 0) ext4_report_failure("write back", node->name);
    fs = saved;
    mutex_unlock(&ext4_lock);
    if (used && write_extents(volume, extents, used) != 0) {
        mutex_lock(&ext4_lock);
        saved = ext4_enter(volume);
        ext4_failed_stage = "data write";
        ext4_report_failure("write back", node->name);
        fs = saved;
        mutex_unlock(&ext4_lock);
        status = -1;
    }
    mutex_unlock(&ext4_io_lock);
    kfree(extents);
    return status;
}

static int map_read(uint32_t ino, uint64_t first, uint32_t pages, uint64_t *blocks,
                    uint8_t *const *targets, int *inline_done) {
    struct ext4_inode inode;
    if (inode_read(ino, &inode) != 0) return -1;
    uint32_t per_page = VFS_PAGE_SIZE / fs->block_size;
    uint64_t first_block = first * per_page;
    uint64_t count = (uint64_t)pages * per_page;
    if (inode.i_flags & EXT4_INLINE_DATA_FL) {
        for (uint32_t page = 0; page < pages; page++) memset(targets[page], 0, VFS_PAGE_SIZE);
        if (first == 0 && inline_read(ino, &inode, targets[0], VFS_PAGE_SIZE) != 0) return -1;
        *inline_done = 1;
        return 0;
    }
    for (uint64_t part = 0; part < count;) {
        uint32_t run = 0;
        int unwritten = 0;
        int64_t block = inode_map(ino, &inode, first_block + part, &run, &unwritten);
        if (block < 0) return -1;
        if (!run) run = 1;
        for (uint32_t step = 0; step < run && part < count; step++, part++)
            blocks[part] = block > 0 && !unwritten ? (uint64_t)block + step : 0;
    }
    return 0;
}

static int read_mapped(struct ext4_volume *volume, const uint64_t *blocks, uint32_t count,
                       uint8_t *const *pages, uint8_t *bounce) {
    uint32_t block_size = volume->block_size;
    uint32_t per_page = VFS_PAGE_SIZE / block_size;
    uint32_t per_run = EXT4_RUN_BYTES / block_size;
    for (uint32_t at = 0; at < count;) {
        uint8_t *target = pages[at / per_page] + (size_t)(at % per_page) * block_size;
        if (!blocks[at]) {
            memset(target, 0, block_size);
            at++;
            continue;
        }
        uint32_t run = 1;
        while (at + run < count && run < per_run && blocks[at + run] == blocks[at] + run) run++;
        if (ext4_volume_read(volume, blocks[at], run, bounce) != 0) return -1;
        for (uint32_t part = 0; part < run; part++) {
            uint32_t slot = at + part;
            memcpy(pages[slot / per_page] + (size_t)(slot % per_page) * block_size,
                   bounce + (size_t)part * block_size, block_size);
        }
        at += run;
    }
    return 0;
}

static int fetch_pages(struct vfs_node *node, uint64_t first, uint32_t count,
                       uint8_t *const *pages) {
    if (!count || !pages) return -1;
    uint64_t *blocks =
        (uint64_t *)kmalloc((size_t)count * (VFS_PAGE_SIZE / 1024U) * sizeof(*blocks));
    uint8_t *bounce = (uint8_t *)kmalloc(EXT4_RUN_BYTES);
    int status = -1;
    uint32_t mapped = 0;
    int inline_done = 0;
    struct ext4_volume *volume = NULL;
    if (blocks && bounce) {
        mutex_lock(&ext4_lock);
        volume = ext4_volume_of(node);
        if (volume) {
            struct ext4_volume *saved = ext4_enter(volume);
            status = map_read(node->disk_inode, first, count, blocks, pages, &inline_done);
            mapped = count * (uint32_t)(VFS_PAGE_SIZE / fs->block_size);
            if (fs->buffer_count > fs->buffer_limit) buffers_trim();
            fs = saved;
        }
        mutex_unlock(&ext4_lock);
    }
    if (status == 0 && !inline_done) status = read_mapped(volume, blocks, mapped, pages, bounce);
    kfree(blocks);
    kfree(bounce);
    return status;
}

static int fetch_data(struct vfs_node *node) {
    (void)node;
    return 0;
}

static int fetch_page(struct vfs_node *node, uint64_t index, void *out) {
    uint8_t *page = (uint8_t *)out;
    return fetch_pages(node, index, 1, &page);
}

static int sync_node(struct vfs_node *node) {
    mutex_lock(&ext4_lock);
    struct ext4_volume *volume = ext4_volume_of(node);
    mutex_unlock(&ext4_lock);
    return ext4_sync_volume(volume);
}

static int sync_all(void) { return ext4_commit_volumes(1, 1); }

static int map_blocks(struct vfs_node *node, uint64_t pages, int *block,
                      struct vfs_block_run **runs_out, uint32_t *count_out) {
    if (!pages) return -EINVAL;
    uint32_t capacity = 16;
    struct vfs_block_run *runs = (struct vfs_block_run *)kmalloc(capacity * sizeof(*runs));
    if (!runs) return -ENOMEM;
    uint32_t count = 0;
    int status = 0;
    mutex_lock(&ext4_lock);
    struct ext4_volume *volume = ext4_active_volume_of(node);
    if (!volume || volume->read_only) {
        mutex_unlock(&ext4_lock);
        kfree(runs);
        return -EINVAL;
    }
    struct ext4_volume *saved = ext4_enter(volume);
    struct ext4_inode inode;
    uint32_t per_page = (uint32_t)(VFS_PAGE_SIZE / fs->block_size);
    if (inode_read(node->disk_inode, &inode) != 0 || (inode.i_flags & EXT4_INLINE_DATA_FL))
        status = -EINVAL;
    for (uint64_t page = 0; !status && page < pages; page++) {
        uint64_t first_block = 0;
        for (uint32_t part = 0; part < per_page && !status; part++) {
            uint32_t run = 0;
            int unwritten = 0;
            int64_t mapped =
                inode_map(node->disk_inode, &inode, page * per_page + part, &run, &unwritten);
            if (mapped <= 0) status = -EINVAL;
            else if (part == 0) first_block = (uint64_t)mapped;
            else if ((uint64_t)mapped != first_block + part) status = -EINVAL;
        }
        if (status) break;
        uint64_t sector = first_block * fs->sectors_per_block;
        if (count &&
            runs[count - 1].sector + runs[count - 1].pages * (VFS_PAGE_SIZE / 512U) == sector) {
            runs[count - 1].pages++;
            continue;
        }
        if (count == capacity) {
            struct vfs_block_run *grown =
                (struct vfs_block_run *)kmalloc(capacity * 2U * sizeof(*grown));
            if (!grown) {
                status = -ENOMEM;
                break;
            }
            memcpy(grown, runs, capacity * sizeof(*runs));
            kfree(runs);
            runs = grown;
            capacity *= 2U;
        }
        runs[count].first_page = page;
        runs[count].pages = 1;
        runs[count].sector = sector;
        count++;
    }
    int device = -1;
    for (int index = 0; !status && index < block_device_count(); index++)
        if (block_device_at(index) == volume->device) device = index;
    if (!status && device < 0) status = -ENODEV;
    fs = saved;
    mutex_unlock(&ext4_lock);
    if (status) {
        kfree(runs);
        return status;
    }
    *block = device;
    *runs_out = runs;
    *count_out = count;
    return 0;
}

static const struct vfs_persist_ops persist_ops = {
    .created = event_created,
    .removed = event_removed,
    .moved = event_moved,
    .truncated = event_truncated,
    .meta_changed = event_meta_changed,
    .linked = event_linked,
    .released = event_released,
    .fetch = fetch_data,
    .fetch_page = fetch_page,
    .fetch_pages = fetch_pages,
    .writeback = writeback,
    .sync_node = sync_node,
    .sync_all = sync_all,
    .map_blocks = map_blocks,
};

void ext4_set_persist_ops(void) { vfs_set_persist_ops(&persist_ops); }

static int load_symlink_target(uint32_t ino, struct ext4_inode *inode, char **out) {
    uint64_t size = inode_size_of(inode);
    if (!size || size >= fs->block_size) return -1;
    char *target = (char *)kmalloc((size_t)size + 1U);
    if (!target) return -1;
    if (inode->i_flags & EXT4_INLINE_DATA_FL) {
        if (inline_read(ino, inode, (uint8_t *)target, size) != 0) {
            kfree(target);
            return -1;
        }
    } else if (inode_is_fast_symlink(inode)) {
        memcpy(target, inode->i_block, (size_t)size);
    } else {
        int dirty = 0;
        int64_t block = inode_bmap(ino, inode, 0, 0, &dirty);
        if (block <= 0 || ext4_volume_read(fs, (uint64_t)block, 1, data_buf) != 0) {
            kfree(target);
            return -1;
        }
        memcpy(target, data_buf, (size_t)size);
    }
    target[size] = '\0';
    *out = target;
    return 0;
}

struct link_seen {
    uint32_t ino;
    struct vfs_node *node;
};

static struct link_seen *links_seen;
static uint32_t links_seen_capacity;
static uint32_t links_seen_count;

static struct vfs_node *links_seen_find(uint32_t ino) {
    if (!links_seen_capacity) return NULL;
    uint32_t mask = links_seen_capacity - 1U;
    for (uint32_t at = ino & mask; links_seen[at].ino; at = (at + 1U) & mask)
        if (links_seen[at].ino == ino) return links_seen[at].node;
    return NULL;
}

static void links_seen_insert(struct link_seen *table, uint32_t capacity, uint32_t ino,
                              struct vfs_node *node) {
    uint32_t mask = capacity - 1U;
    uint32_t at = ino & mask;
    while (table[at].ino) at = (at + 1U) & mask;
    table[at].ino = ino;
    table[at].node = node;
}

static int links_seen_add(uint32_t ino, struct vfs_node *node) {
    if ((links_seen_count + 1U) * 2U > links_seen_capacity) {
        uint32_t capacity = links_seen_capacity ? links_seen_capacity * 2U : 64U;
        struct link_seen *table = (struct link_seen *)kmalloc(capacity * sizeof(*table));
        if (!table) return -1;
        memset(table, 0, capacity * sizeof(*table));
        for (uint32_t at = 0; at < links_seen_capacity; at++)
            if (links_seen[at].ino)
                links_seen_insert(table, capacity, links_seen[at].ino, links_seen[at].node);
        if (links_seen) kfree(links_seen);
        links_seen = table;
        links_seen_capacity = capacity;
    }
    links_seen_insert(links_seen, links_seen_capacity, ino, node);
    links_seen_count++;
    return 0;
}

void ext4_links_reset(void) {
    if (links_seen) kfree(links_seen);
    links_seen = NULL;
    links_seen_capacity = 0;
    links_seen_count = 0;
}

struct load_item {
    uint32_t ino;
    struct vfs_node *node;
};

struct load_queue {
    struct load_item *items;
    size_t head;
    size_t count;
    size_t capacity;
};

static int load_queue_push(struct load_queue *queue, uint32_t ino, struct vfs_node *node) {
    if (queue->head + queue->count == queue->capacity) {
        size_t capacity = queue->capacity ? queue->capacity * 2 : 64;
        struct load_item *items = (struct load_item *)kmalloc(capacity * sizeof(*items));
        if (!items) return -1;
        if (queue->count) memcpy(items, queue->items + queue->head, queue->count * sizeof(*items));
        kfree(queue->items);
        queue->items = items;
        queue->capacity = capacity;
        queue->head = 0;
    }
    queue->items[queue->head + queue->count].ino = ino;
    queue->items[queue->head + queue->count].node = node;
    queue->count++;
    return 0;
}

static void load_xattrs(uint32_t ino, const struct ext4_inode *inode, struct vfs_node *node) {
    if (!inode_file_acl(inode) &&
        inode->i_extra_isize + EXT4_GOOD_OLD_INODE_SIZE + 4U > fs->inode_size)
        return;
    struct ext4_xattr_set set;
    if (xattr_load(ino, inode, &set) != 0) return;
    if (set.count) xattr_to_vfs(node, &set);
    xattr_set_free(&set);
    node->xattr_dirty = 0;
}

static struct vfs_node *attach(struct vfs_node *dir_node, const char *name, uint32_t flags) {
    struct vfs_node *node = vfs_alloc_node(name, flags);
    if (!node) return NULL;
    if (vfs_attach(dir_node, node) != 0) {
        vfs_free_node(node);
        return NULL;
    }
    return node;
}

static int load_entry(struct vfs_node *dir_node, const struct ext4_dir_entry *entry,
                      struct load_queue *queue) {
    uint32_t child_ino = entry->inode;
    size_t name_len = entry->name_len;
    if (!child_ino || !name_len || name_len > VFS_NAME_MAX) return 0;
    if (entry->name[0] == '.' && (name_len == 1U || (name_len == 2U && entry->name[1] == '.')))
        return 0;
    char name[VFS_NAME_MAX + 1];
    memcpy(name, entry->name, name_len);
    name[name_len] = '\0';

    struct ext4_inode child;
    if (inode_read(child_ino, &child) != 0) return 0;
    if (child.i_flags & (EXT4_ENCRYPT_FL | EXT4_EA_INODE_FL)) return 0;
    uint16_t format = inode_kind(&child);
    struct vfs_node *node = NULL;

    if (format != EXT4_S_IFDIR && child.i_links_count > 1U) {
        struct vfs_node *first = links_seen_find(child_ino);
        if (first) return vfs_attach_link(dir_node, name, first) ? 1 : 0;
    }

    switch (format) {
    case EXT4_S_IFDIR:
        node = attach(dir_node, name, VFS_DIRECTORY);
        if (!node) return 0;
        if (load_queue_push(queue, child_ino, node) != 0) return -1;
        break;
    case EXT4_S_IFREG:
        node = attach(dir_node, name, VFS_FILE);
        if (!node) return 0;
        node->length = inode_size_of(&child);
        if (node->length) node->flags |= VFS_LAZY_DATA;
        vfs_setup_memory_file(node);
        break;
    case EXT4_S_IFLNK: {
        char *target = NULL;
        if (load_symlink_target(child_ino, &child, &target) != 0) return 0;
        node = attach(dir_node, name, VFS_SYMLINK);
        if (!node) {
            kfree(target);
            return 0;
        }
        node->data = target;
        node->length = inode_size_of(&child);
        node->capacity = node->length + 1U;
        node->flags |= VFS_OWNED_DATA;
        break;
    }
    case EXT4_S_IFIFO:  node = attach(dir_node, name, VFS_PIPE); break;
    case EXT4_S_IFSOCK: node = attach(dir_node, name, VFS_SOCKET); break;
    case EXT4_S_IFCHR:
    case EXT4_S_IFBLK:
        node = attach(dir_node, name, format == EXT4_S_IFCHR ? VFS_CHARDEVICE : VFS_BLOCKDEVICE);
        if (node) decode_device(&child, &node->dev_major, &node->dev_minor);
        break;
    default: return 0;
    }
    if (!node) return 0;
    ext4_restore_owner(node, &child);
    adopt(node, child_ino);
    if (fs->read_only) node->flags |= VFS_READONLY;
    load_xattrs(child_ino, &child, node);
    if (child.i_links_count > 1U && format != EXT4_S_IFDIR) links_seen_add(child_ino, node);
    return 1;
}

static int load_entries(struct vfs_node *dir_node, const uint8_t *data, uint32_t size,
                        struct load_queue *queue) {
    int restored = 0;
    uint32_t at = 0;
    while (at + 8U <= size) {
        const struct ext4_dir_entry *entry = (const struct ext4_dir_entry *)(data + at);
        if (!dirent_sane(entry, at, size)) return -1;
        at += entry->rec_len;
        int loaded = load_entry(dir_node, entry, queue);
        if (loaded < 0) return -1;
        restored += loaded;
    }
    return restored;
}

static int load_directory(uint32_t dir_ino, struct vfs_node *dir_node, struct load_queue *queue) {
    struct ext4_inode dir;
    if (inode_read(dir_ino, &dir) != 0) return -1;
    if (dir.i_flags & EXT4_ENCRYPT_FL) return 0;
    if (dir.i_flags & EXT4_INLINE_DATA_FL) {
        uint8_t *entries;
        uint32_t length;
        uint32_t parent;
        if (inline_dir_entries(dir_ino, &dir, &entries, &length, &parent) != 0) return -1;
        int restored = load_entries(dir_node, entries, length, queue);
        kfree(entries);
        return restored;
    }
    uint8_t *block_data = (uint8_t *)kmalloc(fs->block_size);
    if (!block_data) return -1;
    int restored = 0;
    uint64_t block_count = inode_size_of(&dir) / fs->block_size;
    for (uint64_t lblk = 0; lblk < block_count; lblk++) {
        int dirty = 0;
        int64_t block = inode_bmap(dir_ino, &dir, lblk, 0, &dirty);
        if (block < 0) goto fail;
        if (!block) continue;
        if (ext4_volume_read(fs, (uint64_t)block, 1, block_data) != 0) goto fail;
        int loaded = load_entries(dir_node, block_data, fs->block_size, queue);
        if (loaded < 0) goto fail;
        restored += loaded;
    }
    kfree(block_data);
    return restored;

fail:
    kfree(block_data);
    return -1;
}

int ext4_load_tree(uint32_t root_ino, struct vfs_node *root) {
    struct load_queue queue = {NULL, 0, 0, 0};
    int restored = 0;
    struct ext4_inode inode;
    if (inode_read(root_ino, &inode) == 0) load_xattrs(root_ino, &inode, root);
    if (load_queue_push(&queue, root_ino, root) != 0) return -1;
    while (queue.count) {
        struct load_item item = queue.items[queue.head++];
        queue.count--;
        int loaded = load_directory(item.ino, item.node, &queue);
        if (loaded < 0) {
            restored = -1;
            break;
        }
        restored += loaded;
    }
    kfree(queue.items);
    return restored;
}
