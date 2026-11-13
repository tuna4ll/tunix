#include "ext4.h"

#include <tunix/heap.h>
#include <tunix/kstring.h>

#define INLINE_BLOCK_BYTES (EXT4_N_BLOCKS * 4U)
#define INLINE_DIR_PARENT  4U

static struct ext4_xattr *data_item(struct ext4_xattr_set *set) {
    return xattr_set_find(set, EXT4_XATTR_INDEX_SYSTEM, "data");
}

uint64_t inline_size(uint32_t ino, const struct ext4_inode *inode) {
    (void)ino;
    return inode_size_of(inode);
}

int inline_read(uint32_t ino, const struct ext4_inode *inode, uint8_t *out, uint64_t capacity) {
    uint64_t size = inode_size_of(inode);
    if (size > capacity) size = capacity;
    uint64_t first = size < INLINE_BLOCK_BYTES ? size : INLINE_BLOCK_BYTES;
    memcpy(out, inode->i_block, (size_t)first);
    if (size <= first) return 0;
    struct ext4_xattr_set set;
    if (xattr_load(ino, inode, &set) != 0) return -1;
    struct ext4_xattr *item = data_item(&set);
    uint64_t rest = size - first;
    int status = -1;
    if (item && item->value_size >= rest) {
        memcpy(out + first, item->value, (size_t)rest);
        status = 0;
    }
    xattr_set_free(&set);
    return status;
}

int inline_dir_entries(uint32_t ino, const struct ext4_inode *inode, uint8_t **data,
                       uint32_t *length, uint32_t *parent) {
    *data = NULL;
    *length = 0;
    *parent = inode->i_block[0];
    struct ext4_xattr_set set;
    if (xattr_load(ino, inode, &set) != 0) return -1;
    struct ext4_xattr *item = data_item(&set);
    uint32_t extra = item ? item->value_size : 0;
    uint32_t total = INLINE_BLOCK_BYTES - INLINE_DIR_PARENT + extra;
    uint8_t *buffer = (uint8_t *)kmalloc(total);
    if (!buffer) {
        xattr_set_free(&set);
        return -1;
    }
    memcpy(buffer, (const uint8_t *)inode->i_block + INLINE_DIR_PARENT,
           INLINE_BLOCK_BYTES - INLINE_DIR_PARENT);
    if (extra) memcpy(buffer + INLINE_BLOCK_BYTES - INLINE_DIR_PARENT, item->value, extra);
    xattr_set_free(&set);
    *data = buffer;
    *length = total;
    return 0;
}

static int expand_directory(uint32_t ino, struct ext4_inode *inode) {
    uint8_t *entries;
    uint32_t length;
    uint32_t parent;
    if (inline_dir_entries(ino, inode, &entries, &length, &parent) != 0) return -1;
    uint8_t *block = (uint8_t *)kmalloc(fs->block_size);
    if (!block) {
        kfree(entries);
        return -1;
    }
    memset(block, 0, fs->block_size);
    uint8_t type = has_incompat(EXT4_INCOMPAT_FILETYPE) ? EXT4_FT_DIR : 0;
    struct ext4_dir_entry *dot = (struct ext4_dir_entry *)block;
    dot->inode = ino;
    dot->rec_len = 12;
    dot->name_len = 1;
    dot->file_type = type;
    dot->name[0] = '.';
    struct ext4_dir_entry *dotdot = (struct ext4_dir_entry *)(block + 12);
    dotdot->inode = parent;
    dotdot->rec_len = 12;
    dotdot->name_len = 2;
    dotdot->file_type = type;
    dotdot->name[0] = '.';
    dotdot->name[1] = '.';
    uint32_t usable = has_csum() ? fs->block_size - 12U : fs->block_size;
    uint32_t at = 24;
    struct ext4_dir_entry *last = dotdot;
    for (uint32_t offset = 0; offset + 8U <= length;) {
        struct ext4_dir_entry *entry = (struct ext4_dir_entry *)(entries + offset);
        if (!dirent_sane(entry, offset, length)) break;
        if (entry->inode && entry->name_len) {
            uint32_t size = dirent_rec_len(entry->name_len);
            if (at + size > usable) {
                kfree(entries);
                kfree(block);
                return -1;
            }
            memcpy(block + at, entry, 8U + entry->name_len);
            last = (struct ext4_dir_entry *)(block + at);
            last->rec_len = (uint16_t)size;
            at += size;
        }
        offset += entry->rec_len;
    }
    last->rec_len = (uint16_t)(last->rec_len + usable - at);
    if (has_csum()) dir_init_tail(block);
    kfree(entries);

    struct ext4_xattr_set set;
    if (xattr_load(ino, inode, &set) != 0) {
        kfree(block);
        return -1;
    }
    xattr_set_remove(&set, EXT4_XATTR_INDEX_SYSTEM, "data");
    inode->i_flags &= ~EXT4_INLINE_DATA_FL;
    if (has_incompat(EXT4_INCOMPAT_EXTENTS)) ext_init_root(inode);
    else memset(inode->i_block, 0, sizeof(inode->i_block));
    int status = xattr_store(ino, inode, &set);
    xattr_set_free(&set);
    int dirty = 0;
    int64_t physical = status == 0 ? inode_bmap(ino, inode, 0, 1, &dirty) : -1;
    if (physical <= 0) {
        kfree(block);
        return -1;
    }
    dir_block_seal(inode_seed(ino, inode), block);
    status = buf_write((uint64_t)physical, block);
    kfree(block);
    inode_set_size(inode, fs->block_size);
    return status;
}

static int expand_file(uint32_t ino, struct ext4_inode *inode) {
    uint64_t size = inode_size_of(inode);
    if (size > fs->block_size) return -1;
    uint8_t *data = (uint8_t *)kmalloc(fs->block_size);
    if (!data) return -1;
    memset(data, 0, fs->block_size);
    if (size && inline_read(ino, inode, data, size) != 0) {
        kfree(data);
        return -1;
    }
    struct ext4_xattr_set set;
    if (xattr_load(ino, inode, &set) != 0) {
        kfree(data);
        return -1;
    }
    xattr_set_remove(&set, EXT4_XATTR_INDEX_SYSTEM, "data");
    inode->i_flags &= ~EXT4_INLINE_DATA_FL;
    if (has_incompat(EXT4_INCOMPAT_EXTENTS)) ext_init_root(inode);
    else memset(inode->i_block, 0, sizeof(inode->i_block));
    int status = xattr_store(ino, inode, &set);
    xattr_set_free(&set);
    if (status == 0 && size) {
        int dirty = 0;
        int64_t physical = inode_bmap(ino, inode, 0, 1, &dirty);
        if (physical > 0) buf_forget((uint64_t)physical);
        status = physical > 0 ? ext4_volume_write(fs, (uint64_t)physical, 1, data) : -1;
    }
    kfree(data);
    return status;
}

int inline_expand(uint32_t ino, struct ext4_inode *inode) {
    if (!(inode->i_flags & EXT4_INLINE_DATA_FL)) return 0;
    int status =
        inode_kind(inode) == EXT4_S_IFDIR ? expand_directory(ino, inode) : expand_file(ino, inode);
    if (status != 0) return -1;
    return inode_write(ino, inode);
}
