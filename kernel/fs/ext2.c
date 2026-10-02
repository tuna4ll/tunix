#include <stddef.h>
#include <stdint.h>
#include "../include/block.h"
#include "../include/build_config.h"
#include "../include/ext2.h"
#include "../include/ext3.h"
#include "../include/heap.h"
#include "../include/kstring.h"
#include "../include/random.h"
#include "../include/time.h"
#include "../include/vfs.h"

static void vfs_guard_release(int *unused) {
    (void)unused;
    vfs_lock_release();
}

#define VFS_GUARD \
    __attribute__((cleanup(vfs_guard_release))) int vfs_guard = (vfs_lock_acquire(), 0)

extern void kprintf(const char *fmt, ...);

#if TUNIX_DEBUG_LOGS
#define KDEBUG(...) kprintf(__VA_ARGS__)
#else
#define KDEBUG(...) do { } while (0)
#endif

#define EIO 5
#define ENOMEM 12
#define EBUSY 16
#define ENODEV 19
#define EINVAL 22

#define EXT2_MAX_BLOCK_SIZE 4096U
#define EXT2_MAGIC 0xEF53U
#define EXT2_GOOD_OLD_INODE_SIZE 128U
#define EXT2_GROUP_DESC_SIZE 32U
#define EXT2_ROOT_INO 2U
#define EXT2_DIRECT_BLOCKS 12U
#define EXT2_INDIRECT_LEVELS 3U
#define EXT2_LINK_MAX 65000U
#define EXT2_EXTRA_ISIZE 32U

#define EXT2_S_IFREG 0x8000U
#define EXT2_S_IFDIR 0x4000U
#define EXT2_S_IFLNK 0xA000U

#define EXT2_FT_REG_FILE 1U
#define EXT2_FT_DIR 2U
#define EXT2_FT_SYMLINK 7U

#define EXT2_INDEX_FL 0x00001000U

#define EXT2_FEATURE_COMPAT_HAS_JOURNAL 0x0004U
#define EXT2_FEATURE_INCOMPAT_FILETYPE 0x0002U
#define EXT2_FEATURE_INCOMPAT_RECOVER 0x0004U
#define EXT2_FEATURE_RO_COMPAT_SPARSE_SUPER 0x0001U
#define EXT2_FEATURE_RO_COMPAT_LARGE_FILE 0x0002U
#define EXT2_FEATURE_RO_COMPAT_DIR_NLINK 0x0020U
#define EXT2_FEATURE_RO_COMPAT_EXTRA_ISIZE 0x0040U
#define EXT2_INCOMPAT_WRITABLE (EXT2_FEATURE_INCOMPAT_FILETYPE | \
                                EXT2_FEATURE_INCOMPAT_RECOVER)
#define EXT2_RO_COMPAT_WRITABLE (EXT2_FEATURE_RO_COMPAT_SPARSE_SUPER | \
                                 EXT2_FEATURE_RO_COMPAT_LARGE_FILE | \
                                 EXT2_FEATURE_RO_COMPAT_DIR_NLINK | \
                                 EXT2_FEATURE_RO_COMPAT_EXTRA_ISIZE)

#define EXT2_SB_MIN_EXTRA_ISIZE 0x15CU
#define EXT2_SB_WANT_EXTRA_ISIZE 0x15EU
#define EXT2_XATTR_MAGIC 0xEA020000U
#define EXT2_RUN_BYTES (32U * EXT2_MAX_BLOCK_SIZE)

struct ext2_superblock {
    uint32_t s_inodes_count;
    uint32_t s_blocks_count;
    uint32_t s_r_blocks_count;
    uint32_t s_free_blocks_count;
    uint32_t s_free_inodes_count;
    uint32_t s_first_data_block;
    uint32_t s_log_block_size;
    uint32_t s_log_frag_size;
    uint32_t s_blocks_per_group;
    uint32_t s_frags_per_group;
    uint32_t s_inodes_per_group;
    uint32_t s_mtime;
    uint32_t s_wtime;
    uint16_t s_mnt_count;
    uint16_t s_max_mnt_count;
    uint16_t s_magic;
    uint16_t s_state;
    uint16_t s_errors;
    uint16_t s_minor_rev_level;
    uint32_t s_lastcheck;
    uint32_t s_checkinterval;
    uint32_t s_creator_os;
    uint32_t s_rev_level;
    uint16_t s_def_resuid;
    uint16_t s_def_resgid;
    uint32_t s_first_ino;
    uint16_t s_inode_size;
    uint16_t s_block_group_nr;
    uint32_t s_feature_compat;
    uint32_t s_feature_incompat;
    uint32_t s_feature_ro_compat;
    uint8_t s_uuid[16];
    char s_volume_name[16];
    char s_last_mounted[64];
    uint32_t s_algorithm_usage_bitmap;
    uint8_t s_prealloc_blocks;
    uint8_t s_prealloc_dir_blocks;
    uint16_t s_reserved_gdt_blocks;
    uint8_t s_journal_uuid[16];
    uint32_t s_journal_inum;
    uint32_t s_journal_dev;
    uint32_t s_last_orphan;
    uint8_t s_reserved[788];
} __attribute__((packed));

struct ext2_group_desc {
    uint32_t bg_block_bitmap;
    uint32_t bg_inode_bitmap;
    uint32_t bg_inode_table;
    uint16_t bg_free_blocks_count;
    uint16_t bg_free_inodes_count;
    uint16_t bg_used_dirs_count;
    uint16_t bg_pad;
    uint32_t bg_reserved[3];
} __attribute__((packed));

struct ext2_inode {
    uint16_t i_mode;
    uint16_t i_uid;
    uint32_t i_size;
    uint32_t i_atime;
    uint32_t i_ctime;
    uint32_t i_mtime;
    uint32_t i_dtime;
    uint16_t i_gid;
    uint16_t i_links_count;
    uint32_t i_blocks;
    uint32_t i_flags;
    uint32_t i_osd1;
    uint32_t i_block[15];
    uint32_t i_generation;
    uint32_t i_file_acl;
    uint32_t i_dir_acl;
    uint32_t i_faddr;
    uint8_t i_osd2[12];
} __attribute__((packed));

struct ext2_dirent {
    uint32_t inode;
    uint16_t rec_len;
    uint8_t name_len;
    uint8_t file_type;
    char name[];
} __attribute__((packed));

typedef char ext2_superblock_size_check[(sizeof(struct ext2_superblock) == 1024) ? 1 : -1];
typedef char ext2_group_desc_size_check[(sizeof(struct ext2_group_desc) == EXT2_GROUP_DESC_SIZE) ? 1 : -1];
typedef char ext2_inode_size_check[(sizeof(struct ext2_inode) == EXT2_GOOD_OLD_INODE_SIZE) ? 1 : -1];

struct block_cache {
    uint32_t block;
    int dirty;
    uint8_t data[EXT2_MAX_BLOCK_SIZE];
};

struct ext2_volume {
    struct ext2_volume *next;
    const struct block_device *device;
    struct vfs_node *root;
    int loading;

    struct ext2_superblock sb;
    uint8_t *sb_home;
    uint32_t sb_home_block;
    uint32_t sb_home_offset;
    struct ext2_group_desc *gds;
    uint32_t group_count;
    uint32_t first_data_block;
    uint32_t blocks_per_group;
    uint32_t inodes_per_group;
    uint32_t gd_blocks;

    uint32_t block_size;
    uint32_t sectors_per_block;
    uint32_t inode_size;
    uint32_t inodes_per_block;
    uint32_t pointers;
    uint32_t gd_per_block;
    uint16_t extra_isize;

    struct block_cache bbitmap;
    struct block_cache ibitmap;
    struct block_cache itable;
    struct block_cache dir;
    struct block_cache map[EXT2_INDIRECT_LEVELS];

    uint32_t block_cursor_group;
    uint32_t block_cursor_bit;
    uint32_t inode_cursor_group;
    uint32_t inode_cursor_bit;

    struct ext3_journal *journal;
    struct ext2_inode journal_inode;
};

static struct ext2_volume *volumes;
static struct ext2_volume *fs;

static uint8_t meta_buf[EXT2_MAX_BLOCK_SIZE];
static uint8_t data_buf[EXT2_MAX_BLOCK_SIZE];
static uint8_t bulk_buf[EXT2_RUN_BYTES];

static struct ext2_volume *enter(struct ext2_volume *volume) {
    struct ext2_volume *previous = fs;
    fs = volume;
    return previous;
}

static struct ext2_volume *volume_of(const struct vfs_node *node) {
    if (!node || !node->disk_inode || !node->fs_private) return NULL;
    for (struct ext2_volume *volume = volumes; volume; volume = volume->next)
        if (volume == node->fs_private) return volume;
    return NULL;
}

static struct ext2_volume *active_volume_of(const struct vfs_node *node) {
    struct ext2_volume *volume = volume_of(node);
    return volume && !volume->loading ? volume : NULL;
}

static void adopt(struct vfs_node *node, uint32_t ino) {
    node->disk_inode = ino;
    node->fs_private = fs;
}

static void forget(struct vfs_node *node) {
    vfs_forget_backing(node);
    node->fs_private = NULL;
}

static int journal_active(void) {
    return ext3_journal_active(fs->journal);
}

static uint16_t sb_u16(uint32_t offset) {
    uint16_t value;
    memcpy(&value, (const uint8_t *)&fs->sb + offset, sizeof(value));
    return value;
}

static uint32_t gd_blocks_for(uint32_t groups) {
    return (groups + fs->gd_per_block - 1U) / fs->gd_per_block;
}

static uint32_t group_first_block(uint32_t group) {
    return fs->first_data_block + group * fs->blocks_per_group;
}

static uint32_t group_block_count(uint32_t group) {
    uint32_t first = group_first_block(group);
    if (first >= fs->sb.s_blocks_count) return 0;
    uint32_t remaining = fs->sb.s_blocks_count - first;
    return remaining < fs->blocks_per_group ? remaining : fs->blocks_per_group;
}

static uint32_t epoch32(void) {
    return (uint32_t)time_epoch_seconds();
}

static void restore_times(struct vfs_node *node, const struct ext2_inode *inode) {
    node->atime = inode->i_atime;
    node->mtime = inode->i_mtime;
    node->ctime = inode->i_ctime;
}

static int volume_read(struct ext2_volume *volume, uint32_t block, uint32_t count,
                       void *out) {
    return block_device_read(volume->device, (uint64_t)block * volume->sectors_per_block,
                             count * volume->sectors_per_block, out);
}

static int volume_write(struct ext2_volume *volume, uint32_t block, uint32_t count,
                        const void *data) {
    return block_device_write(volume->device, (uint64_t)block * volume->sectors_per_block,
                              count * volume->sectors_per_block, data);
}

static int volume_flush(struct ext2_volume *volume) {
    return block_device_flush(volume->device);
}

static int read_blocks_raw(uint32_t block, uint32_t count, void *out) {
    return volume_read(fs, block, count, out);
}

static int read_blocks(uint32_t block, uint32_t count, void *out) {
    if (!ext3_journal_staged(fs->journal)) return read_blocks_raw(block, count, out);
    uint8_t *bytes = (uint8_t *)out;
    for (uint32_t index = 0; index < count; index++) {
        uint8_t *slot = bytes + (size_t)index * fs->block_size;
        if (ext3_journal_peek(fs->journal, block + index, slot) == 0) continue;
        if (read_blocks_raw(block + index, 1, slot) != 0) return -1;
    }
    return 0;
}

static int write_blocks(uint32_t block, uint32_t count, const void *data) {
    return volume_write(fs, block, count, data);
}

static int read_block(uint32_t block, void *out) {
    return read_blocks(block, 1, out);
}

static int meta_write(uint32_t block, const void *data) {
    if (journal_active()) return ext3_journal_stage(fs->journal, block, data);
    return write_blocks(block, 1, data);
}

static int cache_flush_one(struct block_cache *cache) {
    if (cache->block && cache->dirty &&
        meta_write(cache->block, cache->data) != 0) return -1;
    cache->dirty = 0;
    return 0;
}

static uint8_t *cache_get(struct block_cache *cache, uint32_t block) {
    if (cache->block == block) return cache->data;
    if (cache_flush_one(cache) != 0) return NULL;
    if (read_block(block, cache->data) != 0) {
        cache->block = 0;
        return NULL;
    }
    cache->block = block;
    return cache->data;
}

static uint8_t *cache_put_new(struct block_cache *cache, uint32_t block) {
    if (cache_flush_one(cache) != 0) return NULL;
    memset(cache->data, 0, fs->block_size);
    cache->block = block;
    cache->dirty = 1;
    return cache->data;
}

static void cache_invalidate(struct block_cache *cache) {
    cache->block = 0;
    cache->dirty = 0;
}

static void invalidate_maps(void) {
    for (uint32_t level = 0; level < EXT2_INDIRECT_LEVELS; level++)
        cache_invalidate(&fs->map[level]);
    cache_invalidate(&fs->dir);
}

static int cache_flush_all(void) {
    struct block_cache *caches[] = {
        &fs->bbitmap, &fs->ibitmap, &fs->itable, &fs->dir,
        &fs->map[0], &fs->map[1], &fs->map[2],
    };
    int status = 0;
    for (size_t index = 0; index < sizeof(caches) / sizeof(caches[0]); index++)
        if (cache_flush_one(caches[index]) != 0) status = -1;
    return status;
}

static void cache_reset_all(void) {
    cache_invalidate(&fs->bbitmap);
    cache_invalidate(&fs->ibitmap);
    cache_invalidate(&fs->itable);
    invalidate_maps();
}

static int write_gd_table(void) {
    uint32_t first_block = fs->first_data_block + 1U;
    for (uint32_t index = 0; index < fs->gd_blocks; index++) {
        uint32_t start = index * fs->gd_per_block;
        uint32_t count = fs->group_count - start;
        if (count > fs->gd_per_block) count = fs->gd_per_block;
        memset(meta_buf, 0, fs->block_size);
        memcpy(meta_buf, &fs->gds[start], count * sizeof(struct ext2_group_desc));
        if (meta_write(first_block + index, meta_buf) != 0) return -1;
    }
    return 0;
}

static void sb_home_refresh(void) {
    memcpy(fs->sb_home + fs->sb_home_offset, &fs->sb, sizeof(fs->sb));
}

static int write_superblock_direct(void) {
    sb_home_refresh();
    return write_blocks(fs->sb_home_block, 1, fs->sb_home);
}

static int flush_meta(void) {
    if (cache_flush_all() != 0) return -1;
    int journalled = journal_active();
    if (journalled) fs->sb.s_feature_incompat |= EXT2_FEATURE_INCOMPAT_RECOVER;
    sb_home_refresh();
    if (meta_write(fs->sb_home_block, fs->sb_home) != 0) return -1;
    if (write_gd_table() != 0) return -1;
    if (!journalled) return 0;
    return ext3_journal_commit(fs->journal);
}

static int64_t bitmap_scan(const uint8_t *bits, uint32_t start, uint32_t max_bits) {
    const uint32_t *words = (const uint32_t *)bits;
    if (start >= max_bits) start = 0;
    for (int pass = 0; pass < 2; pass++) {
        uint32_t bit = pass ? 0 : start;
        uint32_t end = pass ? start : max_bits;
        while (bit < end) {
            uint32_t index = bit >> 5;
            uint32_t word = words[index] | ((1U << (bit & 31U)) - 1U);
            if (word == 0xFFFFFFFFU) {
                bit = (index + 1U) << 5;
                continue;
            }
            uint32_t found = (index << 5) + (uint32_t)__builtin_ctz(~word);
            if (found >= end) break;
            return (int64_t)found;
        }
    }
    return -1;
}

static int64_t bitmap_alloc(struct block_cache *cache, uint32_t bitmap_block,
                            uint32_t max_bits, uint32_t start) {
    uint8_t *bits = cache_get(cache, bitmap_block);
    if (!bits) return -1;
    int64_t bit = bitmap_scan(bits, start, max_bits);
    if (bit < 0) return -1;
    bits[bit >> 3] |= (uint8_t)(1U << (bit & 7U));
    cache->dirty = 1;
    return bit;
}

static void bitmap_release(struct block_cache *cache, uint32_t bitmap_block,
                           uint32_t bit) {
    uint8_t *bits = cache_get(cache, bitmap_block);
    if (!bits) return;
    bits[bit >> 3] &= (uint8_t)~(1U << (bit & 7U));
    cache->dirty = 1;
}

static uint32_t alloc_block(void) {
    uint32_t groups = fs->group_count;
    if (fs->block_cursor_group >= groups) fs->block_cursor_group = fs->block_cursor_bit = 0;
    for (uint32_t index = 0; index < groups; index++) {
        uint32_t group = fs->block_cursor_group + index;
        if (group >= groups) group -= groups;
        if (!fs->gds[group].bg_free_blocks_count) continue;
        uint32_t start = group == fs->block_cursor_group ? fs->block_cursor_bit : 0;
        int64_t bit = bitmap_alloc(&fs->bbitmap, fs->gds[group].bg_block_bitmap,
                                   group_block_count(group), start);
        if (bit < 0) continue;
        if (fs->sb.s_free_blocks_count) fs->sb.s_free_blocks_count--;
        fs->gds[group].bg_free_blocks_count--;
        fs->block_cursor_group = group;
        fs->block_cursor_bit = (uint32_t)bit + 1U;
        return group_first_block(group) + (uint32_t)bit;
    }
    kprintf("EXT2: %s is out of blocks\n", fs->device->dev_name);
    return 0;
}

static void free_block(uint32_t block) {
    if (block < fs->first_data_block || block >= fs->sb.s_blocks_count) return;
    uint32_t within = block - fs->first_data_block;
    uint32_t group = within / fs->blocks_per_group;
    bitmap_release(&fs->bbitmap, fs->gds[group].bg_block_bitmap,
                   within % fs->blocks_per_group);
    fs->sb.s_free_blocks_count++;
    fs->gds[group].bg_free_blocks_count++;
}

static uint32_t alloc_inode(void) {
    uint32_t groups = fs->group_count;
    if (fs->inode_cursor_group >= groups) fs->inode_cursor_group = fs->inode_cursor_bit = 0;
    for (uint32_t index = 0; index < groups; index++) {
        uint32_t group = fs->inode_cursor_group + index;
        if (group >= groups) group -= groups;
        if (!fs->gds[group].bg_free_inodes_count) continue;
        uint32_t start = group == fs->inode_cursor_group ? fs->inode_cursor_bit : 0;
        int64_t bit = bitmap_alloc(&fs->ibitmap, fs->gds[group].bg_inode_bitmap,
                                   fs->inodes_per_group, start);
        if (bit < 0) continue;
        if (fs->sb.s_free_inodes_count) fs->sb.s_free_inodes_count--;
        fs->gds[group].bg_free_inodes_count--;
        fs->inode_cursor_group = group;
        fs->inode_cursor_bit = (uint32_t)bit + 1U;
        return group * fs->inodes_per_group + (uint32_t)bit + 1U;
    }
    kprintf("EXT2: %s is out of inodes\n", fs->device->dev_name);
    return 0;
}

static void free_inode(uint32_t ino, int is_directory) {
    if (!ino || ino > fs->sb.s_inodes_count) return;
    uint32_t index = ino - 1U;
    uint32_t group = index / fs->inodes_per_group;
    bitmap_release(&fs->ibitmap, fs->gds[group].bg_inode_bitmap,
                   index % fs->inodes_per_group);
    fs->sb.s_free_inodes_count++;
    fs->gds[group].bg_free_inodes_count++;
    if (is_directory && fs->gds[group].bg_used_dirs_count)
        fs->gds[group].bg_used_dirs_count--;
}

static void inode_group_dirs_inc(uint32_t ino) {
    if (!ino || ino > fs->sb.s_inodes_count) return;
    fs->gds[(ino - 1U) / fs->inodes_per_group].bg_used_dirs_count++;
}

static uint8_t *inode_slot(uint32_t ino) {
    if (!ino || ino > fs->sb.s_inodes_count) return NULL;
    uint32_t index = ino - 1U;
    uint32_t group = index / fs->inodes_per_group;
    uint32_t within = index % fs->inodes_per_group;
    uint32_t block = fs->gds[group].bg_inode_table + within / fs->inodes_per_block;
    uint8_t *table = cache_get(&fs->itable, block);
    if (!table) return NULL;
    return table + (within % fs->inodes_per_block) * fs->inode_size;
}

static int inode_read(uint32_t ino, struct ext2_inode *out) {
    uint8_t *slot = inode_slot(ino);
    if (!slot) return -1;
    memcpy(out, slot, sizeof(*out));
    return 0;
}

static int inode_write(uint32_t ino, const struct ext2_inode *in) {
    uint8_t *slot = inode_slot(ino);
    if (!slot) return -1;
    memcpy(slot, in, sizeof(*in));
    fs->itable.dirty = 1;
    return 0;
}

static int inode_write_new(uint32_t ino, const struct ext2_inode *in) {
    uint8_t *slot = inode_slot(ino);
    if (!slot) return -1;
    memset(slot, 0, fs->inode_size);
    memcpy(slot, in, sizeof(*in));
    if (fs->extra_isize) {
        memcpy(slot + EXT2_GOOD_OLD_INODE_SIZE, &fs->extra_isize, sizeof(fs->extra_isize));
        if (fs->extra_isize >= 20U) {
            uint32_t now = epoch32();
            memcpy(slot + EXT2_GOOD_OLD_INODE_SIZE + 16U, &now, sizeof(now));
        }
    }
    fs->itable.dirty = 1;
    return 0;
}

static int is_regular(const struct ext2_inode *inode) {
    return (inode->i_mode & 0xF000U) == EXT2_S_IFREG;
}

static uint64_t inode_size_of(const struct ext2_inode *inode) {
    uint64_t size = inode->i_size;
    if (is_regular(inode) && fs->sb.s_rev_level >= 1)
        size |= (uint64_t)inode->i_dir_acl << 32;
    return size;
}

static void inode_set_size(struct ext2_inode *inode, uint64_t size) {
    if (fs->sb.s_rev_level < 1 && size > 0x7FFFFFFFULL) size = 0x7FFFFFFFULL;
    inode->i_size = (uint32_t)size;
    if (!is_regular(inode) || fs->sb.s_rev_level < 1) return;
    inode->i_dir_acl = (uint32_t)(size >> 32);
    if (size > 0x7FFFFFFFULL)
        fs->sb.s_feature_ro_compat |= EXT2_FEATURE_RO_COMPAT_LARGE_FILE;
}

static void inode_links_adjust(uint32_t ino, int delta) {
    struct ext2_inode inode;
    if (inode_read(ino, &inode) != 0) return;
    int directory = (inode.i_mode & 0xF000U) == EXT2_S_IFDIR;
    if (directory && inode.i_links_count == 1U) return;
    if (delta < 0 && inode.i_links_count) {
        inode.i_links_count--;
    } else if (delta > 0) {
        if (inode.i_links_count + 1U < EXT2_LINK_MAX) {
            inode.i_links_count++;
        } else if (directory) {
            inode.i_links_count = 1;
            fs->sb.s_feature_ro_compat |= EXT2_FEATURE_RO_COMPAT_DIR_NLINK;
        }
    }
    inode_write(ino, &inode);
}

static void count_block(struct ext2_inode *inode, int *inode_dirty) {
    inode->i_blocks += fs->sectors_per_block;
    *inode_dirty = 1;
}

static void uncount_block(struct ext2_inode *inode) {
    if (inode->i_blocks >= fs->sectors_per_block) inode->i_blocks -= fs->sectors_per_block;
}

static int64_t inode_bmap(struct ext2_inode *inode, uint32_t file_block,
                          int alloc, int *inode_dirty) {
    if (file_block < EXT2_DIRECT_BLOCKS) {
        uint32_t block = inode->i_block[file_block];
        if (block || !alloc) return block;
        block = alloc_block();
        if (!block) return -1;
        inode->i_block[file_block] = block;
        count_block(inode, inode_dirty);
        return block;
    }

    uint64_t index = file_block - EXT2_DIRECT_BLOCKS;
    uint64_t span = fs->pointers;
    uint32_t level = 1;
    while (index >= span) {
        index -= span;
        if (++level > EXT2_INDIRECT_LEVELS) return -1;
        span *= fs->pointers;
    }

    uint32_t top = EXT2_DIRECT_BLOCKS - 1U + level;
    uint32_t block = inode->i_block[top];
    if (!block) {
        if (!alloc) return 0;
        block = alloc_block();
        if (!block || !cache_put_new(&fs->map[0], block)) return -1;
        inode->i_block[top] = block;
        count_block(inode, inode_dirty);
    }
    for (uint32_t depth = 0; depth < level; depth++) {
        span /= fs->pointers;
        uint32_t slot = (uint32_t)(index / span);
        index %= span;
        struct block_cache *cache = &fs->map[depth];
        uint32_t *entries = (uint32_t *)cache_get(cache, block);
        if (!entries) return -1;
        uint32_t child = entries[slot];
        if (!child) {
            if (!alloc) return 0;
            child = alloc_block();
            if (!child) return -1;
            if (depth + 1U < level && !cache_put_new(&fs->map[depth + 1U], child))
                return -1;
            entries = (uint32_t *)cache_get(cache, block);
            if (!entries) return -1;
            entries[slot] = child;
            cache->dirty = 1;
            count_block(inode, inode_dirty);
        }
        block = child;
    }
    return block;
}

static void free_subtree(uint32_t block, uint32_t height, struct ext2_inode *inode) {
    uint32_t *entries = (uint32_t *)kmalloc(fs->block_size);
    if (entries && read_block(block, entries) == 0) {
        for (uint32_t slot = 0; slot < fs->pointers; slot++) {
            if (!entries[slot]) continue;
            if (height > 1U) {
                free_subtree(entries[slot], height - 1U, inode);
            } else {
                free_block(entries[slot]);
                uncount_block(inode);
            }
        }
    }
    kfree(entries);
    free_block(block);
    uncount_block(inode);
}

static int trim_subtree(uint32_t block, uint32_t height, uint64_t base, uint64_t keep,
                        struct ext2_inode *inode) {
    if (keep <= base) {
        free_subtree(block, height, inode);
        return 1;
    }
    uint64_t cover = 1;
    for (uint32_t level = 1; level < height; level++) cover *= fs->pointers;
    if (base + cover * fs->pointers <= keep) return 0;
    uint32_t *entries = (uint32_t *)kmalloc(fs->block_size);
    if (!entries) return 0;
    if (read_block(block, entries) != 0) {
        kfree(entries);
        return 0;
    }
    int changed = 0;
    for (uint32_t slot = 0; slot < fs->pointers; slot++) {
        if (!entries[slot]) continue;
        uint64_t child_base = base + slot * cover;
        if (child_base + cover <= keep) continue;
        if (height == 1U) {
            free_block(entries[slot]);
            uncount_block(inode);
        } else if (!trim_subtree(entries[slot], height - 1U, child_base, keep, inode)) {
            continue;
        }
        entries[slot] = 0;
        changed = 1;
    }
    if (changed) meta_write(block, entries);
    kfree(entries);
    return 0;
}

static void inode_truncate_blocks(struct ext2_inode *inode, uint64_t keep) {
    cache_flush_all();
    for (uint64_t index = keep; index < EXT2_DIRECT_BLOCKS; index++) {
        if (!inode->i_block[index]) continue;
        free_block(inode->i_block[index]);
        uncount_block(inode);
        inode->i_block[index] = 0;
    }
    uint64_t base = EXT2_DIRECT_BLOCKS;
    uint64_t span = fs->pointers;
    for (uint32_t level = 1; level <= EXT2_INDIRECT_LEVELS; level++) {
        uint32_t top = EXT2_DIRECT_BLOCKS - 1U + level;
        if (inode->i_block[top] &&
            trim_subtree(inode->i_block[top], level, base, keep, inode))
            inode->i_block[top] = 0;
        base += span;
        span *= fs->pointers;
    }
    cache_flush_all();
    invalidate_maps();
}

static void inode_release_blocks(struct ext2_inode *inode) {
    inode_truncate_blocks(inode, 0);
}

static void release_xattr(struct ext2_inode *inode) {
    uint32_t block = inode->i_file_acl;
    inode->i_file_acl = 0;
    if (!block || block >= fs->sb.s_blocks_count) return;
    uint32_t *header = (uint32_t *)kmalloc(fs->block_size);
    if (header && read_block(block, header) == 0 && header[0] == EXT2_XATTR_MAGIC &&
        header[1] > 1U) {
        header[1]--;
        meta_write(block, header);
    } else {
        free_block(block);
    }
    kfree(header);
    uncount_block(inode);
}

static int inode_is_fast_symlink(const struct ext2_inode *inode) {
    uint32_t xattr = inode->i_file_acl ? fs->sectors_per_block : 0;
    return (inode->i_mode & 0xF000U) == EXT2_S_IFLNK &&
           inode->i_size < 60U && inode->i_blocks == xattr;
}

static uint8_t dirent_type_for(const struct vfs_node *node) {
    uint32_t kind = node->flags & 0xFFU;
    if (kind == VFS_DIRECTORY) return EXT2_FT_DIR;
    if (kind == VFS_SYMLINK) return EXT2_FT_SYMLINK;
    return EXT2_FT_REG_FILE;
}

static void dirent_fill(struct ext2_dirent *entry, uint32_t ino,
                        const char *name, size_t name_len, uint8_t file_type) {
    entry->inode = ino;
    entry->name_len = (uint8_t)name_len;
    entry->file_type = file_type;
    memcpy(entry->name, name, name_len);
}

static int dir_open(uint32_t dir_ino, struct ext2_inode *dir) {
    if (inode_read(dir_ino, dir) != 0) return -1;
    if (!(dir->i_flags & EXT2_INDEX_FL)) return 0;
    dir->i_flags &= ~EXT2_INDEX_FL;
    return inode_write(dir_ino, dir);
}

static int dirent_sane(const struct ext2_dirent *entry, uint32_t at) {
    return entry->rec_len >= 8U && !(entry->rec_len & 3U) &&
           at + entry->rec_len <= fs->block_size &&
           8U + (uint32_t)entry->name_len <= entry->rec_len;
}

static int dir_add_entry(uint32_t dir_ino, const char *name, uint32_t child_ino,
                         uint8_t file_type) {
    size_t name_len = strlen(name);
    if (!name_len || name_len > 255U) return -1;
    uint16_t needed = (uint16_t)(8U + ((name_len + 3U) & ~3U));

    struct ext2_inode dir;
    if (dir_open(dir_ino, &dir) != 0) return -1;
    uint32_t block_count = dir.i_size / fs->block_size;

    for (uint32_t file_block = 0; file_block < block_count; file_block++) {
        int dirty = 0;
        int64_t block = inode_bmap(&dir, file_block, 0, &dirty);
        if (block <= 0) return -1;
        uint8_t *dir_data = cache_get(&fs->dir, (uint32_t)block);
        if (!dir_data) return -1;
        uint32_t at = 0;
        while (at + 8U <= fs->block_size) {
            struct ext2_dirent *entry = (struct ext2_dirent *)(dir_data + at);
            if (!dirent_sane(entry, at)) return -1;
            if (!entry->inode && entry->rec_len >= needed) {
                dirent_fill(entry, child_ino, name, name_len, file_type);
                fs->dir.dirty = 1;
                return 0;
            }
            uint16_t used = (uint16_t)(8U + ((entry->name_len + 3U) & ~3U));
            if (entry->inode && entry->rec_len >= used + needed) {
                struct ext2_dirent *fresh =
                    (struct ext2_dirent *)(dir_data + at + used);
                fresh->rec_len = (uint16_t)(entry->rec_len - used);
                entry->rec_len = used;
                dirent_fill(fresh, child_ino, name, name_len, file_type);
                fs->dir.dirty = 1;
                return 0;
            }
            at += entry->rec_len;
        }
    }

    int dirty = 0;
    int64_t block = inode_bmap(&dir, block_count, 1, &dirty);
    if (block <= 0) return -1;
    uint8_t *dir_data = cache_put_new(&fs->dir, (uint32_t)block);
    if (!dir_data) return -1;
    struct ext2_dirent *entry = (struct ext2_dirent *)dir_data;
    entry->rec_len = (uint16_t)fs->block_size;
    dirent_fill(entry, child_ino, name, name_len, file_type);
    dir.i_size += fs->block_size;
    dir.i_mtime = epoch32();
    return inode_write(dir_ino, &dir);
}

static int dir_remove_entry(uint32_t dir_ino, const char *name) {
    size_t name_len = strlen(name);
    struct ext2_inode dir;
    if (dir_open(dir_ino, &dir) != 0) return -1;
    uint32_t block_count = dir.i_size / fs->block_size;

    for (uint32_t file_block = 0; file_block < block_count; file_block++) {
        int dirty = 0;
        int64_t block = inode_bmap(&dir, file_block, 0, &dirty);
        if (block <= 0) return -1;
        uint8_t *dir_data = cache_get(&fs->dir, (uint32_t)block);
        if (!dir_data) return -1;
        uint32_t at = 0;
        struct ext2_dirent *previous = NULL;
        while (at + 8U <= fs->block_size) {
            struct ext2_dirent *entry = (struct ext2_dirent *)(dir_data + at);
            if (!dirent_sane(entry, at)) return -1;
            if (entry->inode && entry->name_len == name_len &&
                strncmp(entry->name, name, name_len) == 0) {
                if (previous) previous->rec_len += entry->rec_len;
                else entry->inode = 0;
                fs->dir.dirty = 1;
                return 0;
            }
            previous = entry;
            at += entry->rec_len;
        }
    }
    return -1;
}

static int dir_set_dotdot(uint32_t dir_ino, uint32_t parent_ino) {
    struct ext2_inode dir;
    if (dir_open(dir_ino, &dir) != 0) return -1;
    int dirty = 0;
    int64_t block = inode_bmap(&dir, 0, 0, &dirty);
    if (block <= 0) return -1;
    uint8_t *dir_data = cache_get(&fs->dir, (uint32_t)block);
    if (!dir_data) return -1;
    struct ext2_dirent *dot = (struct ext2_dirent *)dir_data;
    if (dot->rec_len < 8U || dot->rec_len >= fs->block_size) return -1;
    struct ext2_dirent *dotdot = (struct ext2_dirent *)(dir_data + dot->rec_len);
    if (dotdot->name_len != 2 || dotdot->name[0] != '.' || dotdot->name[1] != '.')
        return -1;
    dotdot->inode = parent_ino;
    fs->dir.dirty = 1;
    return 0;
}

static int dir_write_initial_block(uint32_t block, uint32_t dir_ino,
                                   uint32_t parent_ino) {
    uint8_t *dir_data = cache_put_new(&fs->dir, block);
    if (!dir_data) return -1;
    struct ext2_dirent *dot = (struct ext2_dirent *)dir_data;
    dot->inode = dir_ino;
    dot->rec_len = 12;
    dot->name_len = 1;
    dot->file_type = EXT2_FT_DIR;
    dot->name[0] = '.';
    struct ext2_dirent *dotdot = (struct ext2_dirent *)(dir_data + 12);
    dotdot->inode = parent_ino;
    dotdot->rec_len = (uint16_t)(fs->block_size - 12U);
    dotdot->name_len = 2;
    dotdot->file_type = EXT2_FT_DIR;
    dotdot->name[0] = '.';
    dotdot->name[1] = '.';
    return 0;
}

struct run_writer {
    uint32_t start_block;
    uint32_t count;
};

static int run_flush(struct run_writer *run) {
    if (!run->count) return 0;
    int status = 0;
    if (journal_active()) {
        for (uint32_t index = 0; index < run->count; index++) {
            status = ext3_journal_stage(fs->journal, run->start_block + index,
                                        bulk_buf + (size_t)index * fs->block_size);
            if (status != 0) break;
        }
    } else {
        status = write_blocks(run->start_block, run->count, bulk_buf);
    }
    run->count = 0;
    return status;
}

static int run_append(struct run_writer *run, uint32_t block, const void *data) {
    uint32_t capacity = EXT2_RUN_BYTES / fs->block_size;
    if (run->count &&
        (block != run->start_block + run->count || run->count == capacity)) {
        if (run_flush(run) != 0) return -1;
    }
    if (!run->count) run->start_block = block;
    memcpy(bulk_buf + (size_t)run->count * fs->block_size, data, fs->block_size);
    run->count++;
    return 0;
}

static const char *failed_stage = "?";

static int write_page(struct ext2_inode *inode, struct vfs_node *node, uint64_t index,
                      const uint8_t *page, struct run_writer *run, int *allocated) {
    uint32_t block_size = fs->block_size;
    for (uint32_t part = 0; part < VFS_PAGE_SIZE / block_size; part++) {
        uint64_t start = index * VFS_PAGE_SIZE + (uint64_t)part * block_size;
        if (start >= node->length) break;
        uint64_t file_block = start / block_size;
        failed_stage = "block map";
        if (file_block > 0xFFFFFFFFULL) return -1;
        int dirty = 0;
        int64_t block = inode_bmap(inode, (uint32_t)file_block, 1, &dirty);
        if (block <= 0) return -1;
        if (dirty) *allocated = 1;
        const uint8_t *source = page + (size_t)part * block_size;
        uint64_t available = node->length - start;
        if (available < block_size) {
            memset(data_buf, 0, block_size);
            memcpy(data_buf, source, (size_t)available);
            source = data_buf;
        }
        failed_stage = "data write";
        if (run_append(run, (uint32_t)block, source) != 0) return -1;
    }
    return 0;
}

static int file_write_range(uint32_t ino, struct vfs_node *node,
                            uint64_t offset, uint64_t size) {
    struct ext2_inode inode;
    failed_stage = "inode read";
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
            if (write_page(&inode, node, index, page, &run, &allocated) != 0) {
                run_flush(&run);
                inode_write(ino, &inode);
                return -1;
            }
            vfs_page_clear_dirty(node, index);
        }
        failed_stage = "data write";
        if (run_flush(&run) != 0) return -1;
    }

    inode_set_size(&inode, node->length);
    inode.i_atime = node->atime;
    inode.i_ctime = node->ctime;
    inode.i_mtime = node->mtime;
    failed_stage = "inode write";
    if (inode_write(ino, &inode) != 0) return -1;
    return allocated;
}

static int under_volatile(const struct vfs_node *node) {
    for (const struct vfs_node *walk = node; walk && walk != fs->root;
         walk = walk->parent)
        if (walk->flags & VFS_VOLATILE) return 1;
    return 0;
}

#define FAILURE_REPORT_LIMIT 8U

static void report_failure(const char *what, const char *name) {
    static unsigned reported;
    if (reported >= FAILURE_REPORT_LIMIT) return;
    reported++;
    kprintf("EXT2: could not %s %s on %s at the %s, %u blocks and %u inodes free%s\n",
            what, name, fs->device->dev_name, failed_stage,
            (unsigned)fs->sb.s_free_blocks_count, (unsigned)fs->sb.s_free_inodes_count,
            reported == FAILURE_REPORT_LIMIT ? " (last report)" : "");
}

static int create_one(struct vfs_node *node) {
    if (!node->parent || volume_of(node->parent) != fs || node->disk_inode) return -1;
    uint32_t kind = node->flags & 0xFFU;
    if (under_volatile(node)) return 1;
    if (node->link_target) return 1;
    if (kind != VFS_FILE && kind != VFS_DIRECTORY && kind != VFS_SYMLINK)
        return 1;

    failed_stage = "inode allocation";
    uint32_t ino = alloc_inode();
    if (!ino) return -1;
    uint32_t parent_ino = node->parent->disk_inode;

    struct ext2_inode inode;
    memset(&inode, 0, sizeof(inode));
    inode.i_uid = (uint16_t)node->uid;
    inode.i_gid = (uint16_t)node->gid;
    inode.i_atime = node->atime;
    inode.i_ctime = node->ctime;
    inode.i_mtime = node->mtime;

    if (kind == VFS_DIRECTORY) {
        failed_stage = "directory block";
        uint32_t block = alloc_block();
        if (!block || dir_write_initial_block(block, ino, parent_ino) != 0) {
            if (block) free_block(block);
            free_inode(ino, 0);
            return -1;
        }
        inode.i_mode = (uint16_t)(EXT2_S_IFDIR | (node->mode & 07777U));
        inode.i_size = fs->block_size;
        inode.i_blocks = fs->sectors_per_block;
        inode.i_block[0] = block;
        inode.i_links_count = 2;
    } else if (kind == VFS_SYMLINK) {
        uint64_t length = node->length;
        inode.i_mode = (uint16_t)(EXT2_S_IFLNK | 0777U);
        inode.i_links_count = 1;
        inode.i_size = (uint32_t)length;
        if (length < 60U) {
            memcpy(inode.i_block, node->data, (size_t)length);
        } else if (length < fs->block_size) {
            failed_stage = "symlink block";
            uint32_t block = alloc_block();
            if (!block) {
                free_inode(ino, 0);
                return -1;
            }
            memset(data_buf, 0, fs->block_size);
            memcpy(data_buf, node->data, (size_t)length);
            if (meta_write(block, data_buf) != 0) {
                free_block(block);
                free_inode(ino, 0);
                return -1;
            }
            inode.i_block[0] = block;
            inode.i_blocks = fs->sectors_per_block;
        } else {
            free_inode(ino, 0);
            return -1;
        }
    } else {
        inode.i_mode = (uint16_t)(EXT2_S_IFREG | (node->mode & 07777U));
        inode.i_links_count = 1;
    }

    failed_stage = "inode write";
    if (inode_write_new(ino, &inode) != 0) {
        free_inode(ino, kind == VFS_DIRECTORY);
        return -1;
    }
    failed_stage = "directory entry";
    if (dir_add_entry(parent_ino, node->name, ino, dirent_type_for(node)) != 0) {
        free_inode(ino, kind == VFS_DIRECTORY);
        return -1;
    }
    if (kind == VFS_DIRECTORY) {
        inode_links_adjust(parent_ino, 1);
        inode_group_dirs_inc(ino);
    }
    adopt(node, ino);
    if (kind == VFS_FILE && node->length)
        file_write_range(ino, node, 0, node->length);
    return 0;
}

static int link_one(struct vfs_node *link) {
    struct vfs_node *target = link->link_target;
    if (!link->parent || volume_of(link->parent) != fs || !target ||
        volume_of(target) != fs) return -1;
    if (dir_add_entry(link->parent->disk_inode, link->name, target->disk_inode,
                      dirent_type_for(target)) != 0) return -1;
    inode_links_adjust(target->disk_inode, 1);
    return 0;
}

static int unlink_one(struct vfs_node *body, uint32_t parent_ino,
                      const char *name) {
    if (!body || !body->disk_inode) return -1;
    dir_remove_entry(parent_ino, name);
    inode_links_adjust(body->disk_inode, -1);
    return 0;
}

static void release_inode(uint32_t ino, int is_directory) {
    struct ext2_inode inode;
    if (inode_read(ino, &inode) != 0) return;
    if (!inode_is_fast_symlink(&inode)) inode_release_blocks(&inode);
    release_xattr(&inode);
    inode.i_links_count = 0;
    inode.i_dtime = epoch32();
    inode_write(ino, &inode);
    free_inode(ino, is_directory);
}

static int remove_one(struct vfs_node *node, uint32_t parent_ino,
                      const char *name) {
    uint32_t ino = node->disk_inode;
    if (!ino) return -1;
    dir_remove_entry(parent_ino, name);

    struct ext2_inode inode;
    if (inode_read(ino, &inode) != 0) return -1;
    int is_directory = (inode.i_mode & 0xF000U) == EXT2_S_IFDIR;
    if (is_directory) inode_links_adjust(parent_ino, -1);
    release_inode(ino, is_directory);
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
    return !node->link_target && volume_of(node) == fs && node->links <= 1 &&
           (node->flags & 0xFFU) == VFS_DIRECTORY && node->children;
}

static void unpersist_one(struct vfs_node *node, uint32_t parent_ino, const char *name) {
    if (node->link_target) {
        if (volume_of(node->link_target) == fs)
            unlink_one(node->link_target, parent_ino, name);
        return;
    }
    if (volume_of(node) != fs) return;
    if (node->links > 1) {
        unlink_one(node, parent_ino, name);
        return;
    }
    if ((node->flags & 0xFFU) != VFS_DIRECTORY && vfs_fault_in(node) != 0) {
        failed_stage = "data read";
        report_failure("move", node->name);
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

static void ext2_event_created(struct vfs_node *node) {
    struct ext2_volume *volume = node ? active_volume_of(node->parent) : NULL;
    if (!volume) return;
    struct ext2_volume *saved = enter(volume);
    int status = create_one(node);
    if (status < 0)
        report_failure("create", node->name);
    else if (status == 0)
        flush_meta();
    fs = saved;
}

static void ext2_event_removed(struct vfs_node *node) {
    struct ext2_volume *volume = node ? active_volume_of(node->parent) : NULL;
    if (!volume) return;
    struct ext2_volume *saved = enter(volume);
    uint32_t parent_ino = node->parent->disk_inode;
    if (node->link_target) {
        if (volume_of(node->link_target) == volume) {
            unlink_one(node->link_target, parent_ino, node->name);
            flush_meta();
        }
    } else if (volume_of(node) == volume) {
        if (node->links > 1 || ((node->flags & 0xFFU) == VFS_FILE && node->refs))
            unlink_one(node, parent_ino, node->name);
        else
            remove_one(node, parent_ino, node->name);
        flush_meta();
    }
    fs = saved;
}

static void ext2_event_linked(struct vfs_node *link) {
    struct ext2_volume *volume = link ? active_volume_of(link->parent) : NULL;
    if (!volume || volume_of(link->link_target) != volume) return;
    struct ext2_volume *saved = enter(volume);
    if (link_one(link) != 0) kprintf("EXT2: cannot link %s\n", link->name);
    else flush_meta();
    fs = saved;
}

static void ext2_event_released(struct vfs_node *node) {
    struct ext2_volume *volume = active_volume_of(node);
    if (!volume) return;
    struct ext2_volume *saved = enter(volume);
    release_inode(node->disk_inode, 0);
    forget(node);
    flush_meta();
    fs = saved;
}

static void move_link(struct vfs_node *node, struct vfs_node *old_parent,
                      const char *old_name) {
    struct vfs_node *target = node->link_target;
    struct ext2_volume *volume = active_volume_of(target);
    if (!volume) return;
    int was = volume_of(old_parent) == volume;
    int now = volume_of(node->parent) == volume;
    if (!was && !now) return;
    struct ext2_volume *saved = enter(volume);
    if (was) dir_remove_entry(old_parent->disk_inode, old_name);
    if (now)
        dir_add_entry(node->parent->disk_inode, node->name, target->disk_inode,
                      dirent_type_for(target));
    if (was != now) inode_links_adjust(target->disk_inode, now ? 1 : -1);
    flush_meta();
    fs = saved;
}

static void ext2_event_moved(struct vfs_node *node, struct vfs_node *old_parent,
                             const char *old_name) {
    if (!node) return;
    if (node->link_target) {
        move_link(node, old_parent, old_name);
        return;
    }
    struct ext2_volume *home = volume_of(node);
    struct ext2_volume *from = volume_of(old_parent);
    struct ext2_volume *to = volume_of(node->parent);
    if ((home && home->loading) || (to && to->loading)) return;

    if (home && home == from && home == to) {
        struct ext2_volume *saved = enter(home);
        uint32_t old_parent_ino = old_parent->disk_inode;
        uint32_t new_parent_ino = node->parent->disk_inode;
        dir_remove_entry(old_parent_ino, old_name);
        dir_add_entry(new_parent_ino, node->name, node->disk_inode,
                      dirent_type_for(node));
        if ((node->flags & 0xFFU) == VFS_DIRECTORY &&
            old_parent_ino != new_parent_ino) {
            dir_set_dotdot(node->disk_inode, new_parent_ino);
            inode_links_adjust(old_parent_ino, -1);
            inode_links_adjust(new_parent_ino, 1);
        }
        flush_meta();
        fs = saved;
        return;
    }
    if (home && home == from) {
        struct ext2_volume *saved = enter(home);
        unpersist_subtree(node, old_parent->disk_inode, old_name);
        flush_meta();
        fs = saved;
    }
    if (to && !node->disk_inode) {
        struct ext2_volume *saved = enter(to);
        persist_subtree(node);
        if ((node->flags & 0xFFU) == VFS_DIRECTORY) persist_links(node);
        flush_meta();
        fs = saved;
    }
}

static void ext2_event_written(struct vfs_node *node, uint64_t offset,
                               uint64_t size) {
    struct ext2_volume *volume = active_volume_of(node);
    if (!volume || (node->flags & 0xFFU) != VFS_FILE || !size) return;
    struct ext2_volume *saved = enter(volume);
    int result = file_write_range(node->disk_inode, node, offset, size);
    if (result < 0)
        report_failure("write back", node->name);
    else if (result > 0)
        flush_meta();
    else
        cache_flush_all();
    fs = saved;
}

static void ext2_event_truncated(struct vfs_node *node) {
    struct ext2_volume *volume = active_volume_of(node);
    if (!volume || (node->flags & 0xFFU) != VFS_FILE) return;
    struct ext2_volume *saved = enter(volume);
    struct ext2_inode inode;
    if (inode_read(node->disk_inode, &inode) == 0) {
        uint64_t keep = (node->length + fs->block_size - 1U) / fs->block_size;
        inode_truncate_blocks(&inode, keep);
        inode_set_size(&inode, node->length);
        inode.i_ctime = node->ctime;
        inode.i_mtime = node->mtime;
        if (inode_write(node->disk_inode, &inode) == 0) {
            if (node->length) file_write_range(node->disk_inode, node, 0, node->length);
            flush_meta();
        }
    }
    fs = saved;
}

static void ext2_event_meta_changed(struct vfs_node *node) {
    struct ext2_volume *volume = active_volume_of(node);
    if (!volume) return;
    struct ext2_volume *saved = enter(volume);
    struct ext2_inode inode;
    if (inode_read(node->disk_inode, &inode) == 0) {
        inode.i_mode = (uint16_t)((inode.i_mode & 0xF000U) | (node->mode & 07777U));
        inode.i_uid = (uint16_t)node->uid;
        inode.i_gid = (uint16_t)node->gid;
        inode.i_ctime = node->ctime;
        inode_write(node->disk_inode, &inode);
        cache_flush_all();
    }
    fs = saved;
}

static int read_page(uint32_t ino, uint64_t index, uint8_t *out) {
    struct ext2_inode inode;
    if (inode_read(ino, &inode) != 0) return -1;
    uint32_t block_size = fs->block_size;
    uint32_t per_page = VFS_PAGE_SIZE / block_size;
    uint64_t first = index * per_page;
    if (first + per_page - 1U > 0xFFFFFFFFULL) return -1;
    uint32_t part = 0;
    while (part < per_page) {
        int dirty = 0;
        int64_t block = inode_bmap(&inode, (uint32_t)(first + part), 0, &dirty);
        if (block < 0) return -1;
        if (block == 0) {
            memset(out + (size_t)part * block_size, 0, block_size);
            part++;
            continue;
        }
        uint32_t run = 1;
        while (part + run < per_page &&
               inode_bmap(&inode, (uint32_t)(first + part + run), 0, &dirty) ==
                   block + run) run++;
        if (read_blocks((uint32_t)block, run, out + (size_t)part * block_size) != 0)
            return -1;
        part += run;
    }
    return 0;
}

static int ext2_fetch_data(struct vfs_node *node) {
    (void)node;
    return 0;
}

static int ext2_fetch_page(struct vfs_node *node, uint64_t index, void *out) {
    struct ext2_volume *volume = volume_of(node);
    if (!volume || !out) return -1;
    struct ext2_volume *saved = enter(volume);
    int status = read_page(node->disk_inode, index, (uint8_t *)out);
    fs = saved;
    return status;
}

static const struct vfs_persist_ops ext2_persist_ops = {
    .created = ext2_event_created,
    .removed = ext2_event_removed,
    .moved = ext2_event_moved,
    .written = ext2_event_written,
    .truncated = ext2_event_truncated,
    .meta_changed = ext2_event_meta_changed,
    .linked = ext2_event_linked,
    .released = ext2_event_released,
    .fetch = ext2_fetch_data,
    .fetch_page = ext2_fetch_page,
};

static int load_symlink_target(struct ext2_inode *inode, char **out) {
    uint32_t size = inode->i_size;
    if (!size || size >= fs->block_size) return -1;
    char *target = (char *)kmalloc(size + 1U);
    if (!target) return -1;
    if (inode_is_fast_symlink(inode)) {
        memcpy(target, inode->i_block, size);
    } else {
        int dirty = 0;
        int64_t block = inode_bmap(inode, 0, 0, &dirty);
        if (block <= 0 || read_block((uint32_t)block, data_buf) != 0) {
            kfree(target);
            return -1;
        }
        memcpy(target, data_buf, size);
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

static void links_seen_insert(struct link_seen *table, uint32_t capacity,
                              uint32_t ino, struct vfs_node *node) {
    uint32_t mask = capacity - 1U;
    uint32_t at = ino & mask;
    while (table[at].ino) at = (at + 1U) & mask;
    table[at].ino = ino;
    table[at].node = node;
}

static int links_seen_add(uint32_t ino, struct vfs_node *node) {
    if ((links_seen_count + 1U) * 2U > links_seen_capacity) {
        uint32_t capacity = links_seen_capacity ? links_seen_capacity * 2U : 64U;
        struct link_seen *table =
            (struct link_seen *)kmalloc(capacity * sizeof(*table));
        if (!table) return -1;
        memset(table, 0, capacity * sizeof(*table));
        for (uint32_t at = 0; at < links_seen_capacity; at++)
            if (links_seen[at].ino)
                links_seen_insert(table, capacity, links_seen[at].ino,
                                  links_seen[at].node);
        if (links_seen) kfree(links_seen);
        links_seen = table;
        links_seen_capacity = capacity;
    }
    links_seen_insert(links_seen, links_seen_capacity, ino, node);
    links_seen_count++;
    return 0;
}

static void links_seen_reset(void) {
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
        if (queue->count)
            memcpy(items, queue->items + queue->head, queue->count * sizeof(*items));
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

static void restore_owner(struct vfs_node *node, const struct ext2_inode *inode) {
    node->mode = inode->i_mode & 07777U;
    node->uid = inode->i_uid;
    node->gid = inode->i_gid;
    restore_times(node, inode);
}

static int load_directory(uint32_t dir_ino, struct vfs_node *dir_node,
                          struct load_queue *queue) {
    struct ext2_inode dir;
    if (inode_read(dir_ino, &dir) != 0) return -1;
    uint8_t *block_data = (uint8_t *)kmalloc(fs->block_size);
    if (!block_data) return -1;

    int restored = 0;
    uint32_t block_count = dir.i_size / fs->block_size;
    for (uint32_t file_block = 0; file_block < block_count; file_block++) {
        int dirty = 0;
        int64_t block = inode_bmap(&dir, file_block, 0, &dirty);
        if (block < 0) goto fail;
        if (!block) continue;
        if (read_block((uint32_t)block, block_data) != 0) goto fail;

        uint32_t at = 0;
        while (at + 8U <= fs->block_size) {
            struct ext2_dirent *entry = (struct ext2_dirent *)(block_data + at);
            if (!dirent_sane(entry, at)) goto fail;
            uint32_t child_ino = entry->inode;
            size_t name_len = entry->name_len;
            at += entry->rec_len;
            if (!child_ino || !name_len || name_len > VFS_NAME_MAX) continue;
            if (entry->name[0] == '.' &&
                (name_len == 1U || (name_len == 2U && entry->name[1] == '.')))
                continue;

            char name[VFS_NAME_MAX + 1];
            memcpy(name, entry->name, name_len);
            name[name_len] = '\0';

            struct ext2_inode child;
            if (inode_read(child_ino, &child) != 0) continue;
            uint16_t format = child.i_mode & 0xF000U;
            struct vfs_node *node = NULL;

            if (format != EXT2_S_IFDIR && child.i_links_count > 1U) {
                struct vfs_node *first = links_seen_find(child_ino);
                if (first) {
                    if (vfs_attach_link(dir_node, name, first)) restored++;
                    continue;
                }
            }

            if (format == EXT2_S_IFDIR) {
                node = vfs_alloc_node(name, VFS_DIRECTORY);
                if (!node || vfs_attach(dir_node, node) != 0) {
                    if (node) vfs_free_node(node);
                    continue;
                }
                restore_owner(node, &child);
                adopt(node, child_ino);
                restored++;
                if (load_queue_push(queue, child_ino, node) != 0) goto fail;
            } else if (format == EXT2_S_IFREG) {
                node = vfs_alloc_node(name, VFS_FILE);
                if (!node || vfs_attach(dir_node, node) != 0) {
                    if (node) vfs_free_node(node);
                    continue;
                }
                node->length = inode_size_of(&child);
                if (node->length) node->flags |= VFS_LAZY_DATA;
                restore_owner(node, &child);
                adopt(node, child_ino);
                vfs_setup_memory_file(node);
                restored++;
            } else if (format == EXT2_S_IFLNK) {
                char *target = NULL;
                if (load_symlink_target(&child, &target) != 0) continue;
                node = vfs_alloc_node(name, VFS_SYMLINK);
                if (!node || vfs_attach(dir_node, node) != 0) {
                    if (node) vfs_free_node(node);
                    kfree(target);
                    continue;
                }
                node->data = target;
                node->length = child.i_size;
                node->capacity = child.i_size + 1U;
                node->flags |= VFS_OWNED_DATA;
                node->uid = child.i_uid;
                node->gid = child.i_gid;
                restore_times(node, &child);
                adopt(node, child_ino);
                restored++;
            }

            if (node && child.i_links_count > 1U)
                links_seen_add(child_ino, node);
        }
    }
    kfree(block_data);
    return restored;

fail:
    kfree(block_data);
    return -1;
}

static int load_tree(uint32_t root_ino, struct vfs_node *root) {
    struct load_queue queue = {NULL, 0, 0, 0};
    int restored = 0;
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

static int journal_read_block(void *context, uint32_t block, void *out) {
    return volume_read((struct ext2_volume *)context, block, 1, out);
}

static int journal_write_block(void *context, uint32_t block, const void *data) {
    return volume_write((struct ext2_volume *)context, block, 1, data);
}

static int journal_write_run(void *context, uint32_t block, uint32_t count,
                             const void *data) {
    return volume_write((struct ext2_volume *)context, block, count, data);
}

static int journal_flush_device(void *context) {
    return volume_flush((struct ext2_volume *)context);
}

static int journal_map_block(void *context, uint32_t file_block, uint32_t *disk_block) {
    struct ext2_volume *saved = enter((struct ext2_volume *)context);
    int dirty = 0;
    int64_t block = inode_bmap(&fs->journal_inode, file_block, 0, &dirty);
    fs = saved;
    if (block <= 0) return -1;
    *disk_block = (uint32_t)block;
    return 0;
}

static int journal_mark_recovery(void *context, int needs_recovery) {
    struct ext2_volume *saved = enter((struct ext2_volume *)context);
    if (needs_recovery) fs->sb.s_feature_incompat |= EXT2_FEATURE_INCOMPAT_RECOVER;
    else fs->sb.s_feature_incompat &= ~EXT2_FEATURE_INCOMPAT_RECOVER;
    int status = write_superblock_direct();
    fs = saved;
    return status;
}

static const struct ext3_journal_ops journal_ops = {
    journal_read_block, journal_write_block, journal_map_block,
    journal_flush_device, journal_mark_recovery, journal_write_run,
};

static int journal_start_up(void) {
    ext3_journal_close(fs->journal);
    fs->journal = NULL;
    if (!(fs->sb.s_feature_compat & EXT2_FEATURE_COMPAT_HAS_JOURNAL))
        return (fs->sb.s_feature_incompat & EXT2_FEATURE_INCOMPAT_RECOVER) ? -1 : 0;
    if (!fs->sb.s_journal_inum ||
        inode_read(fs->sb.s_journal_inum, &fs->journal_inode) != 0) return -1;
    fs->journal = ext3_journal_open(&journal_ops, fs, fs->block_size);
    if (!fs->journal) return -1;
    return ext3_journal_recover(fs->journal);
}

static int superblock_usable(void) {
    const struct ext2_superblock *sb = &fs->sb;
    if (sb->s_magic != EXT2_MAGIC || sb->s_rev_level > 1 || sb->s_log_block_size > 2)
        return 0;
    uint32_t block_size = 1024U << sb->s_log_block_size;
    uint32_t inode_size = sb->s_rev_level ? sb->s_inode_size : EXT2_GOOD_OLD_INODE_SIZE;
    uint64_t usable_blocks = fs->device->sectors / (block_size / BLOCK_SECTOR_SIZE);
    if (inode_size < EXT2_GOOD_OLD_INODE_SIZE || inode_size > block_size ||
        (inode_size & (inode_size - 1U)) ||
        sb->s_first_data_block != (block_size == 1024U ? 1U : 0U) ||
        sb->s_state != 1 || !sb->s_blocks_per_group || !sb->s_inodes_per_group ||
        sb->s_blocks_per_group > 8U * block_size ||
        sb->s_inodes_per_group > 8U * block_size ||
        sb->s_inodes_per_group % (block_size / inode_size) ||
        sb->s_blocks_count > usable_blocks || sb->s_blocks_count <= sb->s_first_data_block)
        return 0;
    if (sb->s_rev_level &&
        ((sb->s_feature_incompat & ~EXT2_INCOMPAT_WRITABLE) ||
         (sb->s_feature_ro_compat & ~EXT2_RO_COMPAT_WRITABLE)))
        return 0;

    uint32_t groups = (sb->s_blocks_count - sb->s_first_data_block +
                       sb->s_blocks_per_group - 1U) / sb->s_blocks_per_group;
    return groups && sb->s_inodes_count == groups * sb->s_inodes_per_group;
}

static void adopt_geometry(void) {
    const struct ext2_superblock *sb = &fs->sb;
    fs->block_size = 1024U << sb->s_log_block_size;
    fs->sectors_per_block = fs->block_size / BLOCK_SECTOR_SIZE;
    fs->inode_size = sb->s_rev_level ? sb->s_inode_size : EXT2_GOOD_OLD_INODE_SIZE;
    fs->inodes_per_block = fs->block_size / fs->inode_size;
    fs->pointers = fs->block_size / 4U;
    fs->gd_per_block = fs->block_size / EXT2_GROUP_DESC_SIZE;
    fs->sb_home_block = 1024U / fs->block_size;
    fs->sb_home_offset = 1024U % fs->block_size;
    fs->first_data_block = sb->s_first_data_block;
    fs->blocks_per_group = sb->s_blocks_per_group;
    fs->inodes_per_group = sb->s_inodes_per_group;
    fs->group_count = (sb->s_blocks_count - fs->first_data_block +
                       fs->blocks_per_group - 1U) / fs->blocks_per_group;
    fs->gd_blocks = gd_blocks_for(fs->group_count);

    fs->extra_isize = 0;
    uint32_t room = fs->inode_size - EXT2_GOOD_OLD_INODE_SIZE;
    if (room >= 4U) {
        uint32_t extra = sb_u16(EXT2_SB_WANT_EXTRA_ISIZE);
        uint32_t least = sb_u16(EXT2_SB_MIN_EXTRA_ISIZE);
        if (extra < least) extra = least;
        if (!extra) extra = EXT2_EXTRA_ISIZE;
        if (extra > room) extra = room;
        fs->extra_isize = (uint16_t)(extra & ~3U);
    }
}

static int read_metadata(void) {
    if (block_device_read(fs->device, 1024U / BLOCK_SECTOR_SIZE, 2, meta_buf) != 0)
        return -EIO;
    memcpy(&fs->sb, meta_buf, sizeof(fs->sb));
    if (!superblock_usable()) return -EINVAL;
    adopt_geometry();

    kfree(fs->sb_home);
    kfree(fs->gds);
    fs->sb_home = (uint8_t *)kmalloc(fs->block_size);
    fs->gds = (struct ext2_group_desc *)kmalloc((size_t)fs->group_count * sizeof(*fs->gds));
    if (!fs->sb_home || !fs->gds) return -ENOMEM;
    if (read_blocks_raw(fs->sb_home_block, 1, fs->sb_home) != 0) return -EIO;

    for (uint32_t index = 0; index < fs->gd_blocks; index++) {
        if (read_blocks_raw(fs->first_data_block + 1U + index, 1, meta_buf) != 0)
            return -EIO;
        uint32_t start = index * fs->gd_per_block;
        uint32_t count = fs->group_count - start;
        if (count > fs->gd_per_block) count = fs->gd_per_block;
        memcpy(&fs->gds[start], meta_buf, count * sizeof(struct ext2_group_desc));
    }
    for (uint32_t group = 0; group < fs->group_count; group++) {
        uint64_t table_end = (uint64_t)fs->gds[group].bg_inode_table +
                             fs->inodes_per_group / fs->inodes_per_block;
        if (fs->gds[group].bg_block_bitmap >= fs->sb.s_blocks_count ||
            fs->gds[group].bg_inode_bitmap >= fs->sb.s_blocks_count ||
            table_end > fs->sb.s_blocks_count) return -EINVAL;
    }
    return 0;
}

static int volume_open(void) {
    cache_reset_all();
    int status = read_metadata();
    if (status != 0) return status;
    if (journal_start_up() != 0) return -EIO;
    cache_reset_all();
    status = read_metadata();
    if (status != 0) return status;
    if (fs->sb.s_feature_incompat & EXT2_FEATURE_INCOMPAT_RECOVER) {
        fs->sb.s_feature_incompat &= ~EXT2_FEATURE_INCOMPAT_RECOVER;
        if (write_superblock_direct() != 0) return -EIO;
    }
    return 0;
}

static int volume_shutdown(void) {
    int status = flush_meta();
    if (fs->journal && ext3_journal_commit(fs->journal) != 0) status = -1;
    fs->sb.s_feature_incompat &= ~EXT2_FEATURE_INCOMPAT_RECOVER;
    if (write_superblock_direct() != 0) status = -1;
    if (volume_flush(fs) != 0) status = -1;
    return status;
}

static void volume_destroy(struct ext2_volume *volume) {
    ext3_journal_close(volume->journal);
    kfree(volume->gds);
    kfree(volume->sb_home);
    kfree(volume);
}

static void volume_unlist(struct ext2_volume *volume) {
    for (struct ext2_volume **link = &volumes; *link; link = &(*link)->next) {
        if (*link != volume) continue;
        *link = volume->next;
        return;
    }
}

static int mount_volume(const struct block_device *device, struct vfs_node *root,
                        struct ext2_volume **out) {
    struct ext2_volume *volume = (struct ext2_volume *)kmalloc(sizeof(*volume));
    if (!volume) return -ENOMEM;
    memset(volume, 0, sizeof(*volume));
    volume->device = device;
    volume->root = root;
    volume->loading = 1;

    struct ext2_volume *saved = enter(volume);
    int status = volume_open();
    if (status == 0) {
        volume->next = volumes;
        volumes = volume;
        adopt(root, EXT2_ROOT_INO);
        struct ext2_inode inode;
        if (inode_read(EXT2_ROOT_INO, &inode) == 0) restore_owner(root, &inode);
        int restored = load_tree(EXT2_ROOT_INO, root);
        links_seen_reset();
        if (restored < 0) {
            kprintf("EXT2: %s could not be read\n", device->dev_name);
            volume_unlist(volume);
            root->disk_inode = 0;
            root->fs_private = NULL;
            status = -EIO;
        } else {
            KDEBUG("EXT2: %s loaded, %d entries (%u/%u blocks free)\n",
                   device->dev_name, restored, (unsigned)fs->sb.s_free_blocks_count,
                   (unsigned)fs->sb.s_blocks_count);
        }
    }
    fs = saved;
    if (status != 0) {
        volume_destroy(volume);
        return status;
    }
    vfs_set_persist_ops(&ext2_persist_ops);
    *out = volume;
    return 0;
}

static const struct {
    const char *path;
    uint32_t mode;
} ext2_volatile_dirs[] = {
    {"/tmp", 01777}, {"/var/tmp", 01777}, {"/run", 0755},
    {"/dev", 0755}, {"/proc", 0555}, {"/sys", 0555},
};

static void mark_volatile_dirs(void) {
    for (size_t index = 0;
         index < sizeof(ext2_volatile_dirs) / sizeof(ext2_volatile_dirs[0]);
         index++) {
        struct vfs_node *node = vfs_lookup(ext2_volatile_dirs[index].path);
        if (!node) {
            node = vfs_mkdir_p(ext2_volatile_dirs[index].path);
            if (!node) continue;
            node->mode = ext2_volatile_dirs[index].mode;
        }
        node->flags |= VFS_VOLATILE;
        if (strcmp(ext2_volatile_dirs[index].path, "/dev") != 0 &&
            strcmp(ext2_volatile_dirs[index].path, "/proc") != 0 &&
            strcmp(ext2_volatile_dirs[index].path, "/sys") != 0)
            vfs_mount_builtin("tmpfs", ext2_volatile_dirs[index].path, "tmpfs", node);
    }
}

int ext2fs_find_label(const char *label) {
    VFS_GUARD;
    if (!label || !*label) return -1;
    int count = block_device_count();
    for (int index = 0; index < count; index++) {
        const struct block_device *device = block_device_at(index);
        if (!device || !device->read) continue;
        struct ext2_superblock probe;
        if (block_device_read_bytes(device, 1024, sizeof probe, &probe) != 0)
            continue;
        if (probe.s_magic != EXT2_MAGIC) continue;
        size_t length = 0;
        while (length < sizeof probe.s_volume_name && probe.s_volume_name[length])
            length++;
        if (strlen(label) != length) continue;
        if (strncmp(probe.s_volume_name, label, length) == 0) return index;
    }
    return -1;
}

int ext2fs_mount_root(void) {
    VFS_GUARD;
    const struct block_device *device = block_root();
    if (!vfs_root || !device || volume_of(vfs_root)) return -1;
    struct ext2_volume *volume;
    if (mount_volume(device, vfs_root, &volume) != 0) return -1;
    mark_volatile_dirs();
    volume->loading = 0;
    return 0;
}

static int devices_overlap(const struct block_device *a, const struct block_device *b) {
    if (a == b) return 1;
    if (a->parent >= 0 && block_device_at(a->parent) == b) return 1;
    return b->parent >= 0 && block_device_at(b->parent) == a;
}

int ext2fs_mount(const char *source, const char *mount_name, struct vfs_node **root_out) {
    VFS_GUARD;
    *root_out = NULL;
    if (!source) return -EINVAL;
    int index = block_device_index_by_name(source);
    const struct block_device *device = index < 0 ? NULL : block_device_at(index);
    if (!device || !device->read) return -ENODEV;
    for (struct ext2_volume *volume = volumes; volume; volume = volume->next)
        if (devices_overlap(volume->device, device)) return -EBUSY;

    struct vfs_node *root = vfs_alloc_node(mount_name, VFS_DIRECTORY);
    if (!root) return -ENOMEM;
    *root_out = root;
    struct ext2_volume *volume;
    int status = mount_volume(device, root, &volume);
    if (status != 0) return status;
    volume->loading = 0;
    return 0;
}

void ext2fs_unmount(struct vfs_node *root) {
    VFS_GUARD;
    struct ext2_volume *volume = volumes;
    while (volume && volume->root != root) volume = volume->next;
    if (!volume) return;
    struct ext2_volume *saved = enter(volume);
    if (volume_shutdown() != 0)
        kprintf("EXT2: %s did not unmount cleanly\n", volume->device->dev_name);
    fs = saved;
    volume_unlist(volume);
    volume_destroy(volume);
}

int ext2fs_owns(const struct vfs_node *node) {
    VFS_GUARD;
    return volume_of(node) != NULL;
}

int ext2fs_journalled(const struct vfs_node *node) {
    VFS_GUARD;
    struct ext2_volume *volume = volume_of(node);
    return volume && volume->journal;
}

int ext2fs_stats(const struct vfs_node *node, struct ext2_fs_stats *out) {
    VFS_GUARD;
    struct ext2_volume *volume = NULL;
    for (const struct vfs_node *walk = node; walk && !volume; walk = walk->parent) {
        volume = volume_of(walk);
        if (walk->parent == walk) break;
    }
    if (!volume || !out) return -1;
    out->block_size = volume->block_size;
    out->blocks = volume->sb.s_blocks_count;
    out->free_blocks = volume->sb.s_free_blocks_count;
    out->reserved_blocks = volume->sb.s_r_blocks_count;
    out->inodes = volume->sb.s_inodes_count;
    out->free_inodes = volume->sb.s_free_inodes_count;
    return 0;
}

int ext2fs_fsync_node(struct vfs_node *node) {
    VFS_GUARD;
    struct ext2_volume *volume = volume_of(node);
    if (!volume) return 0;
    struct ext2_volume *saved = enter(volume);
    int status = 0;
    if ((node->flags & 0xFFU) == VFS_FILE &&
        file_write_range(node->disk_inode, node, 0, node->length) < 0)
        status = -1;
    if (status == 0 && flush_meta() != 0) status = -1;
    if (status == 0) status = volume_flush(volume);
    fs = saved;
    return status;
}

int ext2fs_sync(void) {
    VFS_GUARD;
    int status = 0;
    for (struct ext2_volume *volume = volumes; volume; volume = volume->next) {
        struct ext2_volume *saved = enter(volume);
        if (flush_meta() != 0 || volume_flush(volume) != 0) status = -1;
        fs = saved;
    }
    return status;
}

int ext2fs_shutdown(void) {
    VFS_GUARD;
    int status = 0;
    for (struct ext2_volume *volume = volumes; volume; volume = volume->next) {
        struct ext2_volume *saved = enter(volume);
        if (volume_shutdown() != 0) status = -1;
        fs = saved;
    }
    return status;
}
