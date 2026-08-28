#include <stddef.h>
#include <stdint.h>
#include "../include/block.h"
#include "../include/build_config.h"
#include "../include/ext2.h"
#include "../include/heap.h"
#include "../include/kstring.h"
#include "../include/random.h"
#include "../include/time.h"
#include "../include/vfs.h"

extern void kprintf(const char *fmt, ...);

#if TUNIX_DEBUG_LOGS
#define KDEBUG(...) kprintf(__VA_ARGS__)
#else
#define KDEBUG(...) do { } while (0)
#endif

/*
 * ext2 driver backing the entire root filesystem.
 *
 * The disk region after the initramfs holds a rev-1 ext2 filesystem (4 KiB
 * blocks, 128-byte inodes, dirent file_type) that Linux can mount directly.
 * The disk is the authoritative copy: the initramfs seeds it on first boot
 * and is not read again. Mounting restores only the tree's shape; file
 * contents arrive one file at a time through ext2_fetch_data(). The VFS tree
 * is the cache, and every mutation is mirrored to disk write-through via the
 * persistence hooks. VFS_VOLATILE directories (/tmp, /run, /dev, /proc, /sys,
 * /var/tmp) and everything under them stay RAM-only. s_state is the seed commit marker: 0 while
 * formatting, 1 only once a full seed landed.
 *
 * Metadata blocks go through single-block write-back caches flushed at the
 * end of every VFS operation; file contents move in multi-block DMA runs.
 */

#define EXT2_BLOCK_SIZE 4096U
#define EXT2_SECTORS_PER_BLOCK (EXT2_BLOCK_SIZE / 512U)
#define EXT2_MAGIC 0xEF53U
/* One bitmap is one block, so neither the blocks nor the inodes of a group can
   number more than 8 * block_size. The real figures come from the superblock;
   this is the ceiling they are checked against. */
#define EXT2_GROUP_MAX_BITS (8U * EXT2_BLOCK_SIZE)
#define EXT2_INODE_SIZE 128U
#define EXT2_INODES_PER_BLOCK (EXT2_BLOCK_SIZE / EXT2_INODE_SIZE)
#define EXT2_ROOT_INO 2U
#define EXT2_FIRST_INO 11U

/* 128 groups is 16 GiB, far past anything the ATA driver's 28-bit LBA
   addressing reaches; the descriptor array it implies is 4 KiB of BSS. */
#define EXT2_MAX_GROUPS 128U
/* 32 is sizeof(struct ext2_group_desc), spelled out because the struct is not
   declared yet here; ext2_group_desc_size_check below asserts it. */
#define EXT2_GD_PER_BLOCK (EXT2_BLOCK_SIZE / 32U)


#define EXT2_POINTERS_PER_BLOCK (EXT2_BLOCK_SIZE / 4U)
#define EXT2_DIRECT_BLOCKS 12U

#define EXT2_S_IFREG 0x8000U
#define EXT2_S_IFDIR 0x4000U
#define EXT2_S_IFLNK 0xA000U

#define EXT2_FT_REG_FILE 1U
#define EXT2_FT_DIR 2U
#define EXT2_FT_SYMLINK 7U

#define EXT2_FEATURE_INCOMPAT_FILETYPE 0x0002U
#define EXT2_MAX_DEPTH 64U
#define EXT2_RUN_BLOCKS 32U

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
    uint8_t s_reserved[820];
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
typedef char ext2_group_desc_size_check[(sizeof(struct ext2_group_desc) == 32) ? 1 : -1];
typedef char ext2_inode_size_check[(sizeof(struct ext2_inode) == 128) ? 1 : -1];

static int ext2_mounted_flag;
static int ext2_loading;
static uint32_t ext2_region_lba;
static struct vfs_node *ext2_root;
static struct ext2_superblock sb;
static struct ext2_group_desc gds[EXT2_MAX_GROUPS];
static uint32_t group_count;
/* The geometry the mounted superblock describes; see adopt_geometry(). */
static uint32_t first_data_block;
static uint32_t blocks_per_group;
static uint32_t inodes_per_group;
/* Length of the group descriptor table in blocks. One block covers 128 groups,
   so this is 1 for every size the ATA driver can address, but the layout is
   computed rather than assumed. */
static uint32_t gd_blocks;

static uint8_t meta_buf[EXT2_BLOCK_SIZE];
static uint8_t data_buf[EXT2_BLOCK_SIZE];
static uint8_t walk_buf[EXT2_BLOCK_SIZE];
static uint8_t walk_buf2[EXT2_BLOCK_SIZE];
static uint8_t bulk_buf[EXT2_RUN_BLOCKS * EXT2_BLOCK_SIZE];

/* --- group layout ------------------------------------------------------- */

/*
 * Where a group's metadata is, according to the group itself.
 *
 * The driver used to compute all of this: it made the filesystem, so it knew
 * that every group began with a superblock backup, then the descriptor table,
 * then the two bitmaps and the inode table. mke2fs writes the image now, and
 * it does none of that reliably -- most groups have no backup (sparse_super),
 * and reserved growth blocks push the rest along -- so the descriptors are
 * read and believed instead of checked against a layout of our own.
 */

static uint32_t gd_blocks_for(uint32_t groups) {
    return (groups + EXT2_GD_PER_BLOCK - 1U) / EXT2_GD_PER_BLOCK;
}

static uint32_t group_first_block(uint32_t group) {
    return first_data_block + group * blocks_per_group;
}

/* The last group is short whenever the filesystem does not end on a group
   boundary; everything that walks a bitmap has to respect that. */
static uint32_t group_block_count(uint32_t group) {
    uint32_t first = group_first_block(group);
    if (first >= sb.s_blocks_count) return 0;
    uint32_t remaining = sb.s_blocks_count - first;
    return remaining < blocks_per_group ? remaining : blocks_per_group;
}

static uint32_t group_bbitmap_block(uint32_t group) {
    return gds[group].bg_block_bitmap;
}

static uint32_t group_ibitmap_block(uint32_t group) {
    return gds[group].bg_inode_bitmap;
}

static uint32_t group_itable_block(uint32_t group) {
    return gds[group].bg_inode_table;
}

static uint32_t epoch32(void) {
    return (uint32_t)time_epoch_seconds();
}

/* The on-disk inode has carried timestamps all along; this is the half that
   was missing, so a restored tree reports the times it was saved with. */
static void restore_times(struct vfs_node *node, const struct ext2_inode *inode) {
    node->atime = inode->i_atime;
    node->mtime = inode->i_mtime;
    node->ctime = inode->i_ctime;
}

/* --- block layer -------------------------------------------------------- */

static int read_blocks(uint32_t block, uint32_t count, void *out) {
    uint32_t lba = ext2_region_lba + block * EXT2_SECTORS_PER_BLOCK;
    uint32_t sectors = count * EXT2_SECTORS_PER_BLOCK;
    return block_read(lba, sectors, out);
}

static int write_blocks(uint32_t block, uint32_t count, const void *data) {
    uint32_t lba = ext2_region_lba + block * EXT2_SECTORS_PER_BLOCK;
    uint32_t sectors = count * EXT2_SECTORS_PER_BLOCK;
    return block_write(lba, sectors, data);
}

static int read_block(uint32_t block, void *out) {
    return read_blocks(block, 1, out);
}

static int write_block(uint32_t block, const void *data) {
    return write_blocks(block, 1, data);
}


/* --- single-block write-back caches -------------------------------------- */

struct block_cache {
    uint32_t block; /* 0 = empty; block 0 is the superblock, never cached */
    int dirty;
    uint8_t data[EXT2_BLOCK_SIZE];
};

static struct block_cache cache_bbitmap;
static struct block_cache cache_ibitmap;
static struct block_cache cache_itable;
static struct block_cache cache_dir;
static struct block_cache cache_map;
static struct block_cache cache_map2;

static struct block_cache *const all_caches[] = {
    &cache_bbitmap, &cache_ibitmap, &cache_itable,
    &cache_dir, &cache_map, &cache_map2,
};
#define EXT2_CACHE_COUNT (sizeof(all_caches) / sizeof(all_caches[0]))

static int cache_flush_one(struct block_cache *cache) {
    if (cache->block && cache->dirty &&
        write_block(cache->block, cache->data) != 0) return -1;
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

/* Claim the cache for a freshly allocated block without reading the disk. */
static uint8_t *cache_put_new(struct block_cache *cache, uint32_t block) {
    if (cache_flush_one(cache) != 0) return NULL;
    memset(cache->data, 0, sizeof(cache->data));
    cache->block = block;
    cache->dirty = 1;
    return cache->data;
}

static void cache_invalidate(struct block_cache *cache) {
    cache->block = 0;
    cache->dirty = 0;
}

static int cache_flush_all(void) {
    int status = 0;
    for (size_t index = 0; index < EXT2_CACHE_COUNT; index++) {
        if (cache_flush_one(all_caches[index]) != 0) status = -1;
    }
    return status;
}

static void cache_reset_all(void) {
    for (size_t index = 0; index < EXT2_CACHE_COUNT; index++)
        cache_invalidate(all_caches[index]);
}

/* Write the descriptor table starting at `first_block`, which is the primary
   copy in group 0 and a backup in any other group. */
static int write_gd_table(uint32_t first_block) {
    for (uint32_t index = 0; index < gd_blocks; index++) {
        uint32_t start = index * EXT2_GD_PER_BLOCK;
        uint32_t count = group_count - start;
        if (count > EXT2_GD_PER_BLOCK) count = EXT2_GD_PER_BLOCK;
        memset(meta_buf, 0, sizeof(meta_buf));
        memcpy(meta_buf, &gds[start], count * sizeof(struct ext2_group_desc));
        if (write_block(first_block + index, meta_buf) != 0) return -1;
    }
    return 0;
}

static int flush_meta(void) {
    if (cache_flush_all() != 0) return -1;
    memset(meta_buf, 0, sizeof(meta_buf));
    memcpy(meta_buf + 1024, &sb, sizeof(sb));
    if (write_block(0, meta_buf) != 0) return -1;
    return write_gd_table(first_data_block + 1U);
}

/* --- bitmaps ------------------------------------------------------------ */

/*
 * Find a free bit at or after `start`, wrapping back to the beginning once so
 * that nothing below the cursor is lost.
 *
 * A group bitmap is 32768 bits of which the used ones come in long runs, so the
 * scan reads a word at a time and skips the full ones outright: a bit-by-bit
 * walk touches memory once per bit and does it on every single allocation.
 * Bits within a byte are numbered from the least significant, which is exactly
 * how a little-endian word load orders them, so the first zero of ~word is the
 * first free bit of the word.
 */
static int64_t bitmap_scan(const uint8_t *bits, uint32_t start, uint32_t max_bits) {
    const uint32_t *words = (const uint32_t *)bits;
    if (start >= max_bits) start = 0;
    for (int pass = 0; pass < 2; pass++) {
        uint32_t bit = pass ? 0 : start;
        uint32_t end = pass ? start : max_bits;
        while (bit < end) {
            uint32_t index = bit >> 5;
            /* the bits before the cursor are not ours to take on this pass */
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

/*
 * Allocation resumes where the last one left off rather than restarting at the
 * beginning of the disk. That is not only about the length of the search: the
 * lowest free block is whatever hole the last deletion left, so always taking
 * it scatters a growing file across the whole filesystem, and write_blocks()
 * can then only ever write one block at a time. Carrying on from the previous
 * block hands out consecutive blocks while consecutive blocks exist, which is
 * what lets run_append() fill a 32-block transfer.
 *
 * Nothing is stranded by this: the scan wraps within the group and the group
 * walk wraps too, so a full pass still sees every free block.
 *
 * Block 0 holds the superblock and is marked used at format time, so 0 is
 * never a valid allocation and stays usable as the "no block" sentinel.
 */
static uint32_t block_cursor_group;
static uint32_t block_cursor_bit;

static uint32_t alloc_block(void) {
    /* a format or a mount can leave the cursor pointing past the last group */
    if (block_cursor_group >= group_count) block_cursor_group = block_cursor_bit = 0;
    for (uint32_t index = 0; index < group_count; index++) {
        uint32_t group = block_cursor_group + index;
        if (group >= group_count) group -= group_count;
        if (!gds[group].bg_free_blocks_count) continue;
        uint32_t start = group == block_cursor_group ? block_cursor_bit : 0;
        int64_t bit = bitmap_alloc(&cache_bbitmap, group_bbitmap_block(group),
                                   group_block_count(group), start);
        if (bit < 0) continue;
        if (sb.s_free_blocks_count) sb.s_free_blocks_count--;
        gds[group].bg_free_blocks_count--;
        block_cursor_group = group;
        block_cursor_bit = (uint32_t)bit + 1U;
        return group_first_block(group) + (uint32_t)bit;
    }
    kprintf("EXT2: out of blocks\n");
    return 0;
}

static void free_block(uint32_t block) {
    if (!block || block >= sb.s_blocks_count) return;
    uint32_t within = block - first_data_block;
    uint32_t group = within / blocks_per_group;
    bitmap_release(&cache_bbitmap, group_bbitmap_block(group),
                   within % blocks_per_group);
    sb.s_free_blocks_count++;
    gds[group].bg_free_blocks_count++;
}

static uint32_t inode_cursor_group;
static uint32_t inode_cursor_bit;

static uint32_t alloc_inode(void) {
    if (inode_cursor_group >= group_count) inode_cursor_group = inode_cursor_bit = 0;
    for (uint32_t index = 0; index < group_count; index++) {
        uint32_t group = inode_cursor_group + index;
        if (group >= group_count) group -= group_count;
        if (!gds[group].bg_free_inodes_count) continue;
        uint32_t start = group == inode_cursor_group ? inode_cursor_bit : 0;
        int64_t bit = bitmap_alloc(&cache_ibitmap, group_ibitmap_block(group),
                                   inodes_per_group, start);
        if (bit < 0) continue;
        if (sb.s_free_inodes_count) sb.s_free_inodes_count--;
        gds[group].bg_free_inodes_count--;
        inode_cursor_group = group;
        inode_cursor_bit = (uint32_t)bit + 1U;
        return group * inodes_per_group + (uint32_t)bit + 1U;
    }
    kprintf("EXT2: out of inodes\n");
    return 0;
}

static void free_inode(uint32_t ino, int is_directory) {
    if (!ino || ino > sb.s_inodes_count) return;
    uint32_t index = ino - 1U;
    uint32_t group = index / inodes_per_group;
    bitmap_release(&cache_ibitmap, group_ibitmap_block(group),
                   index % inodes_per_group);
    sb.s_free_inodes_count++;
    gds[group].bg_free_inodes_count++;
    if (is_directory && gds[group].bg_used_dirs_count)
        gds[group].bg_used_dirs_count--;
}

/* e2fsck checks bg_used_dirs_count per group in its pass 5, so the counter has
   to be bumped in the group that actually owns the inode. */
static void inode_group_dirs_inc(uint32_t ino) {
    if (!ino || ino > sb.s_inodes_count) return;
    gds[(ino - 1U) / inodes_per_group].bg_used_dirs_count++;
}

/* --- inode table -------------------------------------------------------- */

/* Inode numbers are global but the tables are per-group, so an inode has to be
   resolved to its group before it can be located within that group's table. */
static uint32_t inode_table_block(uint32_t ino, uint32_t *offset) {
    uint32_t index = ino - 1U;
    uint32_t group = index / inodes_per_group;
    uint32_t within = index % inodes_per_group;
    *offset = (within % EXT2_INODES_PER_BLOCK) * EXT2_INODE_SIZE;
    return group_itable_block(group) + within / EXT2_INODES_PER_BLOCK;
}

static int inode_read(uint32_t ino, struct ext2_inode *out) {
    if (!ino || ino > sb.s_inodes_count) return -1;
    uint32_t offset;
    uint8_t *table = cache_get(&cache_itable, inode_table_block(ino, &offset));
    if (!table) return -1;
    memcpy(out, table + offset, sizeof(*out));
    return 0;
}

static int inode_write(uint32_t ino, const struct ext2_inode *in) {
    if (!ino || ino > sb.s_inodes_count) return -1;
    uint32_t offset;
    uint8_t *table = cache_get(&cache_itable, inode_table_block(ino, &offset));
    if (!table) return -1;
    memcpy(table + offset, in, sizeof(*in));
    cache_itable.dirty = 1;
    return 0;
}

static void inode_links_adjust(uint32_t ino, int delta) {
    struct ext2_inode inode;
    if (inode_read(ino, &inode) != 0) return;
    if (delta < 0 && inode.i_links_count) inode.i_links_count--;
    else if (delta > 0) inode.i_links_count++;
    inode_write(ino, &inode);
}

/* --- block mapping ------------------------------------------------------ */

static uint32_t map_slot_fetch(struct block_cache *cache, uint32_t map_block,
                               uint32_t slot, int alloc, int *error) {
    uint8_t *data = cache_get(cache, map_block);
    if (!data) { *error = 1; return 0; }
    uint32_t *entries = (uint32_t *)data;
    uint32_t value = entries[slot];
    if (value || !alloc) return value;
    value = alloc_block();
    if (!value) { *error = 1; return 0; }
    entries[slot] = value;
    cache->dirty = 1;
    return value;
}

/*
 * Map a file block to a disk block. Returns the disk block, 0 for a hole
 * (when alloc is 0), or -1 on error. *inode_dirty is set when the inode
 * itself changed.
 */
static int64_t inode_bmap(struct ext2_inode *inode, uint32_t file_block,
                          int alloc, int *inode_dirty) {
    int error = 0;
    if (file_block < EXT2_DIRECT_BLOCKS) {
        uint32_t block = inode->i_block[file_block];
        if (block || !alloc) return block;
        block = alloc_block();
        if (!block) return -1;
        inode->i_block[file_block] = block;
        inode->i_blocks += EXT2_SECTORS_PER_BLOCK;
        *inode_dirty = 1;
        return block;
    }

    file_block -= EXT2_DIRECT_BLOCKS;
    if (file_block < EXT2_POINTERS_PER_BLOCK) {
        uint32_t level1 = inode->i_block[12];
        if (!level1) {
            if (!alloc) return 0;
            level1 = alloc_block();
            if (!level1 || !cache_put_new(&cache_map, level1)) return -1;
            inode->i_block[12] = level1;
            inode->i_blocks += EXT2_SECTORS_PER_BLOCK;
            *inode_dirty = 1;
        }
        uint32_t *entries = (uint32_t *)cache_get(&cache_map, level1);
        if (!entries) return -1;
        uint32_t before = entries[file_block];
        uint32_t block = map_slot_fetch(&cache_map, level1, file_block, alloc, &error);
        if (error) return -1;
        if (block && !before) {
            inode->i_blocks += EXT2_SECTORS_PER_BLOCK;
            *inode_dirty = 1;
        }
        return block;
    }

    file_block -= EXT2_POINTERS_PER_BLOCK;
    if (file_block < EXT2_POINTERS_PER_BLOCK * EXT2_POINTERS_PER_BLOCK) {
        uint32_t level1 = inode->i_block[13];
        if (!level1) {
            if (!alloc) return 0;
            level1 = alloc_block();
            if (!level1 || !cache_put_new(&cache_map, level1)) return -1;
            inode->i_block[13] = level1;
            inode->i_blocks += EXT2_SECTORS_PER_BLOCK;
            *inode_dirty = 1;
        }
        uint32_t slot1 = file_block / EXT2_POINTERS_PER_BLOCK;
        uint32_t slot2 = file_block % EXT2_POINTERS_PER_BLOCK;
        uint32_t *level1_entries = (uint32_t *)cache_get(&cache_map, level1);
        if (!level1_entries) return -1;
        uint32_t level2 = level1_entries[slot1];
        if (!level2) {
            if (!alloc) return 0;
            level2 = alloc_block();
            if (!level2 || !cache_put_new(&cache_map2, level2)) return -1;
            level1_entries = (uint32_t *)cache_get(&cache_map, level1);
            if (!level1_entries) return -1;
            level1_entries[slot1] = level2;
            cache_map.dirty = 1;
            inode->i_blocks += EXT2_SECTORS_PER_BLOCK;
            *inode_dirty = 1;
        }
        uint32_t *leaf = (uint32_t *)cache_get(&cache_map2, level2);
        if (!leaf) return -1;
        uint32_t before = leaf[slot2];
        uint32_t block = map_slot_fetch(&cache_map2, level2, slot2, alloc, &error);
        if (error) return -1;
        if (block && !before) {
            inode->i_blocks += EXT2_SECTORS_PER_BLOCK;
            *inode_dirty = 1;
        }
        return block;
    }
    return -1;
}

static void release_map_block(uint32_t map_block) {
    uint32_t *entries = (uint32_t *)walk_buf2;
    if (read_block(map_block, walk_buf2) != 0) return;
    for (uint32_t slot = 0; slot < EXT2_POINTERS_PER_BLOCK; slot++) {
        if (entries[slot]) free_block(entries[slot]);
    }
    free_block(map_block);
}

static void inode_release_blocks(struct ext2_inode *inode) {
    /* raw walks below read from disk, so dirty cached maps must land first */
    cache_flush_all();
    for (uint32_t index = 0; index < EXT2_DIRECT_BLOCKS; index++) {
        if (inode->i_block[index]) free_block(inode->i_block[index]);
    }
    if (inode->i_block[12]) release_map_block(inode->i_block[12]);
    if (inode->i_block[13]) {
        uint32_t *entries = (uint32_t *)walk_buf;
        if (read_block(inode->i_block[13], walk_buf) == 0) {
            for (uint32_t slot = 0; slot < EXT2_POINTERS_PER_BLOCK; slot++) {
                if (entries[slot]) release_map_block(entries[slot]);
            }
        }
        free_block(inode->i_block[13]);
    }
    memset(inode->i_block, 0, sizeof(inode->i_block));
    inode->i_blocks = 0;
    /* the freed blocks may be reused with new roles; drop stale copies */
    cache_invalidate(&cache_map);
    cache_invalidate(&cache_map2);
    cache_invalidate(&cache_dir);
}

static int inode_is_fast_symlink(const struct ext2_inode *inode) {
    return (inode->i_mode & 0xF000U) == EXT2_S_IFLNK &&
           inode->i_size < 60U && inode->i_blocks == 0;
}

/* --- directory entries -------------------------------------------------- */

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

static int dir_add_entry(uint32_t dir_ino, const char *name, uint32_t child_ino,
                         uint8_t file_type) {
    size_t name_len = strlen(name);
    if (!name_len || name_len > 255U) return -1;
    uint16_t needed = (uint16_t)(8U + ((name_len + 3U) & ~3U));

    struct ext2_inode dir;
    if (inode_read(dir_ino, &dir) != 0) return -1;
    uint32_t block_count = dir.i_size / EXT2_BLOCK_SIZE;

    for (uint32_t file_block = 0; file_block < block_count; file_block++) {
        int dirty = 0;
        int64_t block = inode_bmap(&dir, file_block, 0, &dirty);
        if (block <= 0) return -1;
        uint8_t *dir_data = cache_get(&cache_dir, (uint32_t)block);
        if (!dir_data) return -1;
        uint32_t at = 0;
        while (at + 8U <= EXT2_BLOCK_SIZE) {
            struct ext2_dirent *entry = (struct ext2_dirent *)(dir_data + at);
            if (entry->rec_len < 8U || (entry->rec_len & 3U) ||
                at + entry->rec_len > EXT2_BLOCK_SIZE) return -1;
            if (!entry->inode && entry->rec_len >= needed) {
                dirent_fill(entry, child_ino, name, name_len, file_type);
                cache_dir.dirty = 1;
                return 0;
            }
            uint16_t used = (uint16_t)(8U + ((entry->name_len + 3U) & ~3U));
            if (entry->inode && entry->rec_len >= used + needed) {
                struct ext2_dirent *fresh =
                    (struct ext2_dirent *)(dir_data + at + used);
                fresh->rec_len = (uint16_t)(entry->rec_len - used);
                entry->rec_len = used;
                dirent_fill(fresh, child_ino, name, name_len, file_type);
                cache_dir.dirty = 1;
                return 0;
            }
            at += entry->rec_len;
        }
    }

    int dirty = 0;
    int64_t block = inode_bmap(&dir, block_count, 1, &dirty);
    if (block <= 0) return -1;
    uint8_t *dir_data = cache_put_new(&cache_dir, (uint32_t)block);
    if (!dir_data) return -1;
    struct ext2_dirent *entry = (struct ext2_dirent *)dir_data;
    entry->rec_len = EXT2_BLOCK_SIZE;
    dirent_fill(entry, child_ino, name, name_len, file_type);
    dir.i_size += EXT2_BLOCK_SIZE;
    dir.i_mtime = epoch32();
    return inode_write(dir_ino, &dir);
}

static int dir_remove_entry(uint32_t dir_ino, const char *name) {
    size_t name_len = strlen(name);
    struct ext2_inode dir;
    if (inode_read(dir_ino, &dir) != 0) return -1;
    uint32_t block_count = dir.i_size / EXT2_BLOCK_SIZE;

    for (uint32_t file_block = 0; file_block < block_count; file_block++) {
        int dirty = 0;
        int64_t block = inode_bmap(&dir, file_block, 0, &dirty);
        if (block <= 0) return -1;
        uint8_t *dir_data = cache_get(&cache_dir, (uint32_t)block);
        if (!dir_data) return -1;
        uint32_t at = 0;
        struct ext2_dirent *previous = NULL;
        while (at + 8U <= EXT2_BLOCK_SIZE) {
            struct ext2_dirent *entry = (struct ext2_dirent *)(dir_data + at);
            if (entry->rec_len < 8U || (entry->rec_len & 3U) ||
                at + entry->rec_len > EXT2_BLOCK_SIZE) return -1;
            if (entry->inode && entry->name_len == name_len &&
                strncmp(entry->name, name, name_len) == 0) {
                if (previous) previous->rec_len += entry->rec_len;
                else entry->inode = 0;
                cache_dir.dirty = 1;
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
    if (inode_read(dir_ino, &dir) != 0) return -1;
    int dirty = 0;
    int64_t block = inode_bmap(&dir, 0, 0, &dirty);
    if (block <= 0) return -1;
    uint8_t *dir_data = cache_get(&cache_dir, (uint32_t)block);
    if (!dir_data) return -1;
    struct ext2_dirent *dot = (struct ext2_dirent *)dir_data;
    if (dot->rec_len < 8U || dot->rec_len >= EXT2_BLOCK_SIZE) return -1;
    struct ext2_dirent *dotdot = (struct ext2_dirent *)(dir_data + dot->rec_len);
    if (dotdot->name_len != 2 || dotdot->name[0] != '.' || dotdot->name[1] != '.')
        return -1;
    dotdot->inode = parent_ino;
    cache_dir.dirty = 1;
    return 0;
}

static int dir_write_initial_block(uint32_t block, uint32_t dir_ino,
                                   uint32_t parent_ino) {
    uint8_t *dir_data = cache_put_new(&cache_dir, block);
    if (!dir_data) return -1;
    struct ext2_dirent *dot = (struct ext2_dirent *)dir_data;
    dot->inode = dir_ino;
    dot->rec_len = 12;
    dot->name_len = 1;
    dot->file_type = EXT2_FT_DIR;
    dot->name[0] = '.';
    struct ext2_dirent *dotdot = (struct ext2_dirent *)(dir_data + 12);
    dotdot->inode = parent_ino;
    dotdot->rec_len = EXT2_BLOCK_SIZE - 12U;
    dotdot->name_len = 2;
    dotdot->file_type = EXT2_FT_DIR;
    dotdot->name[0] = '.';
    dotdot->name[1] = '.';
    return 0;
}

/* --- file content ------------------------------------------------------- */

struct run_writer {
    uint32_t start_block;
    uint32_t count;
};

static int run_flush(struct run_writer *run) {
    if (!run->count) return 0;
    int status = write_blocks(run->start_block, run->count, bulk_buf);
    run->count = 0;
    return status;
}

static int run_append(struct run_writer *run, uint32_t block, const void *data) {
    if (run->count &&
        (block != run->start_block + run->count || run->count == EXT2_RUN_BLOCKS)) {
        if (run_flush(run) != 0) return -1;
    }
    if (!run->count) run->start_block = block;
    memcpy(bulk_buf + (size_t)run->count * EXT2_BLOCK_SIZE, data, EXT2_BLOCK_SIZE);
    run->count++;
    return 0;
}

/* Returns -1 on error, 1 when blocks were allocated, 0 otherwise. */
static int file_write_range(uint32_t ino, struct vfs_node *node,
                            uint64_t offset, uint64_t size) {
    struct ext2_inode inode;
    if (inode_read(ino, &inode) != 0) return -1;
    int allocated = 0;

    uint64_t end = offset + size;
    if (end > node->length) end = node->length;
    /* a node that never faulted in still matches what is on the disk */
    if (size && end > offset && !(node->flags & VFS_LAZY_DATA)) {
        uint32_t first = (uint32_t)(offset / EXT2_BLOCK_SIZE);
        uint32_t last = (uint32_t)((end - 1U) / EXT2_BLOCK_SIZE);
        struct run_writer run = {0, 0};
        for (uint32_t file_block = first; file_block <= last; file_block++) {
            int dirty = 0;
            int64_t block = inode_bmap(&inode, file_block, 1, &dirty);
            if (block <= 0) return -1;
            if (dirty) allocated = 1;
            uint64_t start = (uint64_t)file_block * EXT2_BLOCK_SIZE;
            uint64_t available = node->length > start ? node->length - start : 0;
            uint32_t chunk = available > EXT2_BLOCK_SIZE ?
                             EXT2_BLOCK_SIZE : (uint32_t)available;
            memset(data_buf, 0, sizeof(data_buf));
            if (chunk && node->data)
                memcpy(data_buf, (const uint8_t *)node->data + start, chunk);
            if (run_append(&run, (uint32_t)block, data_buf) != 0) return -1;
        }
        if (run_flush(&run) != 0) return -1;
    }

    inode.i_size = node->length > 0xFFFFFFFFULL ? 0xFFFFFFFFU
                                                : (uint32_t)node->length;
    inode.i_atime = node->atime;
    inode.i_ctime = node->ctime;
    inode.i_mtime = node->mtime;
    if (inode_write(ino, &inode) != 0) return -1;
    return allocated;
}

/* --- persistence event handlers ----------------------------------------- */

static int ext2_tracks(const struct vfs_node *node) {
    return ext2_mounted_flag && !ext2_loading && node && node->disk_inode;
}

/*
 * Whether a node belongs to one of the RAM-only trees. The flag sits on the
 * directory at the top -- /tmp, /sys and the rest -- and everything below it
 * inherits: asking only about the node itself let a file created in /tmp be
 * written to the disk, and the root filled up with logs a reboot should have
 * taken away.
 */
static int under_volatile(const struct vfs_node *node) {
    for (const struct vfs_node *walk = node; walk; walk = walk->parent) {
        if (walk->flags & VFS_VOLATILE) return 1;
        /* vfs_root is its own parent, so stop rather than spin. */
        if (walk->parent == walk) break;
    }
    return 0;
}

/* Returns 0 on success, 1 for intentionally skipped nodes, -1 on error. */
static int create_one(struct vfs_node *node) {
    if (!node->parent || !node->parent->disk_inode || node->disk_inode) return -1;
    uint32_t kind = node->flags & 0xFFU;
    if (under_volatile(node)) return 1;
    /* A name for a file elsewhere in the tree; the walk comes back for it once
       every inode exists, since the file it names may not yet. */
    if (node->link_target) return 1;
    if (kind != VFS_FILE && kind != VFS_DIRECTORY && kind != VFS_SYMLINK)
        return 1;

    uint32_t ino = alloc_inode();
    if (!ino) return -1;
    uint32_t parent_ino = node->parent->disk_inode;

    struct ext2_inode inode;
    memset(&inode, 0, sizeof(inode));
    inode.i_uid = (uint16_t)node->uid;
    inode.i_gid = (uint16_t)node->gid;
    /* Persist the times the node already carries, so what stat reported before
       the flush is what comes back after a reboot. */
    inode.i_atime = node->atime;
    inode.i_ctime = node->ctime;
    inode.i_mtime = node->mtime;

    if (kind == VFS_DIRECTORY) {
        uint32_t block = alloc_block();
        if (!block || dir_write_initial_block(block, ino, parent_ino) != 0) {
            free_inode(ino, 0);
            return -1;
        }
        inode.i_mode = (uint16_t)(EXT2_S_IFDIR | (node->mode & 07777U));
        inode.i_size = EXT2_BLOCK_SIZE;
        inode.i_blocks = EXT2_SECTORS_PER_BLOCK;
        inode.i_block[0] = block;
        inode.i_links_count = 2;
    } else if (kind == VFS_SYMLINK) {
        uint64_t length = node->length;
        inode.i_mode = (uint16_t)(EXT2_S_IFLNK | 0777U);
        inode.i_links_count = 1;
        inode.i_size = (uint32_t)length;
        if (length < 60U) {
            memcpy(inode.i_block, node->data, (size_t)length);
        } else if (length < EXT2_BLOCK_SIZE) {
            uint32_t block = alloc_block();
            if (!block) {
                free_inode(ino, 0);
                return -1;
            }
            memset(data_buf, 0, sizeof(data_buf));
            memcpy(data_buf, node->data, (size_t)length);
            if (write_block(block, data_buf) != 0) {
                free_inode(ino, 0);
                return -1;
            }
            inode.i_block[0] = block;
            inode.i_blocks = EXT2_SECTORS_PER_BLOCK;
        } else {
            free_inode(ino, 0);
            return -1;
        }
    } else {
        inode.i_mode = (uint16_t)(EXT2_S_IFREG | (node->mode & 07777U));
        inode.i_links_count = 1;
    }

    if (inode_write(ino, &inode) != 0 ||
        dir_add_entry(parent_ino, node->name, ino, dirent_type_for(node)) != 0) {
        free_inode(ino, kind == VFS_DIRECTORY);
        return -1;
    }
    if (kind == VFS_DIRECTORY) {
        inode_links_adjust(parent_ino, 1);
        inode_group_dirs_inc(ino);
    }
    node->disk_inode = ino;
    if (kind == VFS_FILE && node->length)
        file_write_range(ino, node, 0, node->length);
    return 0;
}

/* Give an inode that already exists a further directory entry. */
static int link_one(struct vfs_node *link) {
    struct vfs_node *target = link->link_target;
    if (!link->parent || !link->parent->disk_inode || !target ||
        !target->disk_inode) return -1;
    if (dir_add_entry(link->parent->disk_inode, link->name, target->disk_inode,
                      dirent_type_for(target)) != 0) return -1;
    inode_links_adjust(target->disk_inode, 1);
    return 0;
}

/* Take one name off an inode that has others. The inode, its blocks and its
   contents all stay: what is being removed is the entry, not the file. */
static int unlink_one(struct vfs_node *body, uint32_t parent_ino,
                      const char *name) {
    if (!body || !body->disk_inode) return -1;
    dir_remove_entry(parent_ino, name);
    inode_links_adjust(body->disk_inode, -1);
    return 0;
}

static int remove_one(struct vfs_node *node, uint32_t parent_ino,
                      const char *name) {
    uint32_t ino = node->disk_inode;
    if (!ino) return -1;
    dir_remove_entry(parent_ino, name);

    struct ext2_inode inode;
    if (inode_read(ino, &inode) != 0) return -1;
    int is_directory = (inode.i_mode & 0xF000U) == EXT2_S_IFDIR;
    if (!inode_is_fast_symlink(&inode)) inode_release_blocks(&inode);
    if (is_directory) inode_links_adjust(parent_ino, -1);
    inode.i_links_count = 0;
    inode.i_dtime = epoch32();
    inode_write(ino, &inode);
    free_inode(ino, is_directory);
    node->disk_inode = 0;
    return 0;
}

static int seed_errors;

static void persist_subtree(struct vfs_node *node, unsigned depth) {
    if (depth > EXT2_MAX_DEPTH) {
        seed_errors++;
        return;
    }
    int status = create_one(node);
    if (status) {
        if (status < 0) seed_errors++;
        return;
    }
    if ((node->flags & 0xFFU) != VFS_DIRECTORY) return;
    for (struct vfs_node *child = node->children; child; child = child->next)
        persist_subtree(child, depth + 1U);
}

/* The second pass of a subtree flush: every inode exists by now, so a name
   standing for one can be written whichever order the walk reached them in. */
static void persist_links(struct vfs_node *node, unsigned depth) {
    if (depth > EXT2_MAX_DEPTH) return;
    for (struct vfs_node *child = node->children; child; child = child->next) {
        if (child->link_target) {
            if (link_one(child) != 0) seed_errors++;
        } else if ((child->flags & 0xFFU) == VFS_DIRECTORY &&
                   !(child->flags & VFS_VOLATILE)) {
            persist_links(child, depth + 1U);
        }
    }
}

static void unpersist_subtree(struct vfs_node *node, uint32_t parent_ino,
                              const char *name, unsigned depth) {
    if (node->link_target) {
        unlink_one(node->link_target, parent_ino, name);
        return;
    }
    if (!node->disk_inode || depth > EXT2_MAX_DEPTH) return;
    /* Other names still reach these blocks, so only the entry may go. */
    if (node->links > 1) {
        unlink_one(node, parent_ino, name);
        return;
    }
    if ((node->flags & 0xFFU) == VFS_DIRECTORY) {
        for (struct vfs_node *child = node->children; child; child = child->next)
            unpersist_subtree(child, node->disk_inode, child->name, depth + 1U);
    } else {
        /* the blocks are about to go, so the contents have to come in now */
        vfs_fault_in(node);
    }
    remove_one(node, parent_ino, name);
}

static void ext2_event_created(struct vfs_node *node) {
    if (!ext2_mounted_flag || ext2_loading || !node || !node->parent ||
        !node->parent->disk_inode) return;
    int status = create_one(node);
    if (status < 0)
        kprintf("EXT2: cannot persist %s\n", node->name);
    else if (status == 0)
        flush_meta();
}

/* The node handed over is the entry being removed, which for a hard link owns
   no inode of its own -- so what to do is decided by how many names the file
   has left, not by which of them this is. */
static void ext2_event_removed(struct vfs_node *node) {
    if (!ext2_mounted_flag || ext2_loading || !node || !node->parent ||
        !node->parent->disk_inode) return;
    uint32_t parent_ino = node->parent->disk_inode;

    struct vfs_node *body = node->link_target ? node->link_target : node;
    if (!body->disk_inode) return;
    if (node->link_target || body->links > 1) unlink_one(body, parent_ino, node->name);
    else remove_one(node, parent_ino, node->name);
    flush_meta();
}

static void ext2_event_linked(struct vfs_node *link) {
    if (!ext2_mounted_flag || ext2_loading || !link || !link->link_target) return;
    /* Nothing to write when either end is not on the disk in the first place,
       as under /tmp: the link lives in memory with the file it names. */
    if (!ext2_tracks(link->link_target) || !link->parent ||
        !link->parent->disk_inode) return;
    if (link_one(link) != 0) kprintf("EXT2: cannot link %s\n", link->name);
    else flush_meta();
}

/* The file has lost the last of its names, having outlived its own. */
static void ext2_event_released(struct vfs_node *node) {
    if (!ext2_tracks(node)) return;
    uint32_t ino = node->disk_inode;
    struct ext2_inode inode;
    if (inode_read(ino, &inode) != 0) return;
    if (!inode_is_fast_symlink(&inode)) inode_release_blocks(&inode);
    inode.i_links_count = 0;
    inode.i_dtime = epoch32();
    inode_write(ino, &inode);
    free_inode(ino, 0);
    node->disk_inode = 0;
    flush_meta();
}

static void ext2_event_moved(struct vfs_node *node, struct vfs_node *old_parent,
                             const char *old_name) {
    if (!ext2_mounted_flag || ext2_loading || !node) return;
    uint32_t old_parent_ino = old_parent ? old_parent->disk_inode : 0;
    uint32_t new_parent_ino = node->parent ? node->parent->disk_inode : 0;

    /* Renaming a hard link moves an entry between directories: the count only
       changes when one end of the move is not on the disk. */
    if (node->link_target) {
        struct vfs_node *target = node->link_target;
        if (!ext2_tracks(target)) return;
        if (old_parent_ino) dir_remove_entry(old_parent_ino, old_name);
        if (new_parent_ino)
            dir_add_entry(new_parent_ino, node->name, target->disk_inode,
                          dirent_type_for(target));
        if (!old_parent_ino != !new_parent_ino)
            inode_links_adjust(target->disk_inode, new_parent_ino ? 1 : -1);
        flush_meta();
        return;
    }

    if (node->disk_inode && old_parent_ino && new_parent_ino) {
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
    } else if (node->disk_inode && old_parent_ino && !new_parent_ino) {
        unpersist_subtree(node, old_parent_ino, old_name, 0);
        flush_meta();
    } else if (!node->disk_inode && new_parent_ino) {
        persist_subtree(node, 0);
        if ((node->flags & 0xFFU) == VFS_DIRECTORY) persist_links(node, 0);
        flush_meta();
    }
}

static void ext2_event_written(struct vfs_node *node, uint64_t offset,
                               uint64_t size) {
    if (!ext2_tracks(node) || (node->flags & 0xFFU) != VFS_FILE || !size) return;
    int result = file_write_range(node->disk_inode, node, offset, size);
    if (result < 0)
        kprintf("EXT2: write-back failed for %s\n", node->name);
    else if (result > 0)
        flush_meta();
    else
        cache_flush_all();
}

static void ext2_event_truncated(struct vfs_node *node) {
    if (!ext2_tracks(node) || (node->flags & 0xFFU) != VFS_FILE) return;
    struct ext2_inode inode;
    if (inode_read(node->disk_inode, &inode) != 0) return;
    inode_release_blocks(&inode);
    inode.i_size = 0;
    inode.i_ctime = node->ctime;
    inode.i_mtime = node->mtime;
    if (inode_write(node->disk_inode, &inode) != 0) return;
    if (node->length) file_write_range(node->disk_inode, node, 0, node->length);
    flush_meta();
}

static void ext2_event_meta_changed(struct vfs_node *node) {
    if (!ext2_tracks(node)) return;
    struct ext2_inode inode;
    if (inode_read(node->disk_inode, &inode) != 0) return;
    inode.i_mode = (uint16_t)((inode.i_mode & 0xF000U) | (node->mode & 07777U));
    inode.i_uid = (uint16_t)node->uid;
    inode.i_gid = (uint16_t)node->gid;
    inode.i_ctime = node->ctime;
    inode_write(node->disk_inode, &inode);
    cache_flush_all();
}

static int ext2_fetch_data(struct vfs_node *node);

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
};


/* --- mount / load ------------------------------------------------------- */

static void *load_file_data(struct ext2_inode *inode) {
    uint32_t size = inode->i_size;
    if (!size) return NULL;
    uint8_t *data = (uint8_t *)kmalloc(size);
    if (!data) return NULL;
    uint32_t block_count = (size + EXT2_BLOCK_SIZE - 1U) / EXT2_BLOCK_SIZE;

    /* gather contiguous disk blocks and read them in single DMA runs;
       heap pages cannot be DMA targets, so bounce through bulk_buf */
    uint32_t run_first_file = 0;
    uint32_t run_start = 0;
    uint32_t run_count = 0;
    for (uint32_t file_block = 0; file_block <= block_count; file_block++) {
        int64_t block = 0;
        if (file_block < block_count) {
            int dirty = 0;
            block = inode_bmap(inode, file_block, 0, &dirty);
            if (block < 0) {
                kfree(data);
                return NULL;
            }
        }
        int extends = run_count && block &&
                      (uint32_t)block == run_start + run_count &&
                      run_count < EXT2_RUN_BLOCKS;
        if (run_count && !extends) {
            if (read_blocks(run_start, run_count, bulk_buf) != 0) {
                kfree(data);
                return NULL;
            }
            for (uint32_t at = 0; at < run_count; at++) {
                uint32_t start = (run_first_file + at) * EXT2_BLOCK_SIZE;
                uint32_t chunk = size - start > EXT2_BLOCK_SIZE ?
                                 EXT2_BLOCK_SIZE : size - start;
                memcpy(data + start, bulk_buf + (size_t)at * EXT2_BLOCK_SIZE, chunk);
            }
            run_count = 0;
        }
        if (file_block == block_count) break;
        if (!block) {
            uint32_t start = file_block * EXT2_BLOCK_SIZE;
            uint32_t chunk = size - start > EXT2_BLOCK_SIZE ?
                             EXT2_BLOCK_SIZE : size - start;
            memset(data + start, 0, chunk);
            continue;
        }
        if (!run_count) {
            run_first_file = file_block;
            run_start = (uint32_t)block;
        }
        run_count++;
    }
    return data;
}

/* vfs_fault_in() lands here the first time a restored file is touched. */
static int ext2_fetch_data(struct vfs_node *node) {
    if (!ext2_mounted_flag || !node || !node->disk_inode) return -1;
    struct ext2_inode inode;
    if (inode_read(node->disk_inode, &inode) != 0) return -1;
    void *data = load_file_data(&inode);
    if (!data) return inode.i_size ? -1 : 0;
    node->data = data;
    node->capacity = inode.i_size;
    node->flags |= VFS_OWNED_DATA;
    return 0;
}

static int load_symlink_target(struct ext2_inode *inode, char **out) {
    uint32_t size = inode->i_size;
    if (!size || size >= EXT2_BLOCK_SIZE) return -1;
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

/*
 * Inodes met more than once while the tree is being read back. A second entry
 * for an inode is the second name of one file, not a second file, and loading
 * it as its own node would give the two names separate contents. Only inodes
 * the disk says have several names are tracked, so an ordinary tree pays for
 * nothing.
 */
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
    /* Grown at half full: linear probing falls apart past that. */
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

static int load_directory(uint32_t dir_ino, struct vfs_node *dir_node,
                          unsigned depth) {
    if (depth > EXT2_MAX_DEPTH) return -1;
    struct ext2_inode dir;
    if (inode_read(dir_ino, &dir) != 0) return -1;
    uint8_t *block_data = (uint8_t *)kmalloc(EXT2_BLOCK_SIZE);
    if (!block_data) return -1;

    int restored = 0;
    uint32_t block_count = dir.i_size / EXT2_BLOCK_SIZE;
    for (uint32_t file_block = 0; file_block < block_count; file_block++) {
        int dirty = 0;
        int64_t block = inode_bmap(&dir, file_block, 0, &dirty);
        if (block < 0) goto fail;
        if (!block) continue;
        if (read_block((uint32_t)block, block_data) != 0) goto fail;

        uint32_t at = 0;
        while (at + 8U <= EXT2_BLOCK_SIZE) {
            struct ext2_dirent *entry = (struct ext2_dirent *)(block_data + at);
            if (entry->rec_len < 8U || (entry->rec_len & 3U) ||
                at + entry->rec_len > EXT2_BLOCK_SIZE) goto fail;
            uint32_t child_ino = entry->inode;
            size_t name_len = entry->name_len;
            at += entry->rec_len;
            if (!child_ino || !name_len || name_len > 127U) continue;
            if (entry->name[0] == '.' &&
                (name_len == 1U || (name_len == 2U && entry->name[1] == '.')))
                continue;

            char name[128];
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
                    if (node) kfree(node);
                    continue;
                }
                node->disk_inode = child_ino;
                node->mode = child.i_mode & 07777U;
                node->uid = child.i_uid;
                node->gid = child.i_gid;
                restore_times(node, &child);
                restored++;
                int below = load_directory(child_ino, node, depth + 1U);
                if (below < 0) goto fail;
                restored += below;
            } else if (format == EXT2_S_IFREG) {
                node = vfs_alloc_node(name, VFS_FILE);
                if (!node || vfs_attach(dir_node, node) != 0) {
                    if (node) kfree(node);
                    continue;
                }
                node->length = child.i_size;
                /* contents stay on disk until something asks for them */
                if (child.i_size) node->flags |= VFS_LAZY_DATA;
                node->mode = child.i_mode & 07777U;
                node->uid = child.i_uid;
                node->gid = child.i_gid;
                restore_times(node, &child);
                node->disk_inode = child_ino;
                vfs_setup_memory_file(node);
                restored++;
            } else if (format == EXT2_S_IFLNK) {
                char *target = NULL;
                if (load_symlink_target(&child, &target) != 0) continue;
                node = vfs_alloc_node(name, VFS_SYMLINK);
                if (!node || vfs_attach(dir_node, node) != 0) {
                    if (node) kfree(node);
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
                node->disk_inode = child_ino;
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

/* --- public API --------------------------------------------------------- */

int ext2fs_mounted(void) {
    return ext2_mounted_flag;
}

int ext2fs_owns(struct vfs_node *node) {
    return ext2_mounted_flag && node && node->disk_inode != 0;
}

int ext2fs_stats(struct ext2_fs_stats *out) {
    if (!ext2_mounted_flag || !out) return -1;
    out->block_size = EXT2_BLOCK_SIZE;
    out->blocks = sb.s_blocks_count;
    out->free_blocks = sb.s_free_blocks_count;
    out->reserved_blocks = sb.s_r_blocks_count;
    out->inodes = sb.s_inodes_count;
    out->free_inodes = sb.s_free_inodes_count;
    return 0;
}

int ext2fs_fsync_node(struct vfs_node *node) {
    if (!ext2fs_owns(node)) return 0;
    if ((node->flags & 0xFFU) == VFS_FILE &&
        file_write_range(node->disk_inode, node, 0, node->length) < 0)
        return -1;
    if (flush_meta() != 0) return -1;
    return block_flush();
}

int ext2fs_sync(void) {
    if (!ext2_mounted_flag) return 0;
    if (flush_meta() != 0) return -1;
    return block_flush();
}

/* How many blocks the device can hold past the start of the filesystem. The
   superblock says how many it actually uses; this is only the ceiling that
   claim is checked against. */
static uint32_t region_usable_blocks(uint32_t region_lba) {
    uint32_t disk_sectors = (uint32_t)block_sectors();
    if (!disk_sectors || region_lba >= disk_sectors) return 0;
    uint32_t blocks = (disk_sectors - region_lba) / EXT2_SECTORS_PER_BLOCK;
    if (blocks > EXT2_MAX_GROUPS * EXT2_GROUP_MAX_BITS)
        blocks = EXT2_MAX_GROUPS * EXT2_GROUP_MAX_BITS;
    return blocks;
}

/*
 * What this driver can mount.
 *
 * What is refused here is only what the code genuinely cannot cope with: a
 * block size other than 4 KiB, because every buffer in this file is sized to
 * it; an inode bigger than the classic 128 bytes; a group whose bitmap would
 * not fit in one block; and any incompatible feature but filetype -- extents
 * and 64-bit block numbers most of all. Everything else the superblock says
 * is taken as given rather than compared against a layout of our own.
 */
static int superblock_usable(uint32_t usable_blocks) {
    if (sb.s_magic != EXT2_MAGIC || sb.s_rev_level != 1 ||
        sb.s_log_block_size != 2 || sb.s_inode_size != EXT2_INODE_SIZE ||
        sb.s_first_data_block != 0 || sb.s_state != 1 ||
        !sb.s_blocks_per_group || !sb.s_inodes_per_group ||
        sb.s_blocks_per_group > EXT2_GROUP_MAX_BITS ||
        sb.s_inodes_per_group > EXT2_GROUP_MAX_BITS ||
        sb.s_inodes_per_group % EXT2_INODES_PER_BLOCK ||
        sb.s_blocks_count > usable_blocks ||
        (sb.s_feature_incompat & ~EXT2_FEATURE_INCOMPAT_FILETYPE))
        return 0;

    uint32_t groups = (sb.s_blocks_count - sb.s_first_data_block +
                       sb.s_blocks_per_group - 1U) / sb.s_blocks_per_group;
    if (!groups || groups > EXT2_MAX_GROUPS) return 0;
    if (sb.s_inodes_count != groups * sb.s_inodes_per_group) return 0;
    return 1;
}

/* Adopt the geometry the superblock describes. Only call this once
   superblock_usable() has accepted it. */
static void adopt_geometry(void) {
    first_data_block = sb.s_first_data_block;
    blocks_per_group = sb.s_blocks_per_group;
    inodes_per_group = sb.s_inodes_per_group;
    group_count = (sb.s_blocks_count - first_data_block + blocks_per_group - 1U) /
                  blocks_per_group;
    gd_blocks = gd_blocks_for(group_count);
}

/*
 * Directories that behave like Linux tmpfs mounts: they exist in RAM on
 * every boot but nothing below them is ever written to disk.
 */
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
        /* /dev, /proc and /sys are declared by the drivers that build them,
           which have not run yet. */
        if (strcmp(ext2_volatile_dirs[index].path, "/dev") != 0 &&
            strcmp(ext2_volatile_dirs[index].path, "/proc") != 0 &&
            strcmp(ext2_volatile_dirs[index].path, "/sys") != 0)
            vfs_mount_builtin("tmpfs", ext2_volatile_dirs[index].path, "tmpfs", node);
    }
}

/*
 * Early check (before the memory managers are up) whether the disk holds a
 * fully seeded root filesystem. When it does, the initramfs is not needed
 * at all.
 */
int ext2fs_probe(uint32_t region_lba) {
    uint32_t usable_blocks = region_usable_blocks(region_lba);
    if (!usable_blocks) return -1;
    ext2_region_lba = region_lba;
    if (read_block(0, meta_buf) != 0) return -1;
    memcpy(&sb, meta_buf + 1024, sizeof(sb));
    return superblock_usable(usable_blocks) ? 0 : -1;
}

int ext2fs_mount_root(uint32_t region_lba) {
    if (ext2_mounted_flag || !vfs_root) return -1;
    uint32_t usable_blocks = region_usable_blocks(region_lba);
    if (!usable_blocks) return -1;
    ext2_region_lba = region_lba;
    cache_reset_all();

    if (read_block(0, meta_buf) != 0) return -1;
    memcpy(&sb, meta_buf + 1024, sizeof(sb));
    if (!superblock_usable(usable_blocks)) return -1;
    adopt_geometry();

    for (uint32_t index = 0; index < gd_blocks; index++) {
        if (read_block(first_data_block + 1U + index, meta_buf) != 0) return -1;
        uint32_t start = index * EXT2_GD_PER_BLOCK;
        uint32_t count = group_count - start;
        if (count > EXT2_GD_PER_BLOCK) count = EXT2_GD_PER_BLOCK;
        memcpy(&gds[start], meta_buf, count * sizeof(struct ext2_group_desc));
    }
    /* The descriptors are the layout now, so what is left to check is that
       each one points inside the filesystem: a bitmap block past the end
       would be read straight off whatever follows the partition. */
    for (uint32_t group = 0; group < group_count; group++) {
        uint32_t table_end = gds[group].bg_inode_table +
                             inodes_per_group / EXT2_INODES_PER_BLOCK;
        if (gds[group].bg_block_bitmap >= sb.s_blocks_count ||
            gds[group].bg_inode_bitmap >= sb.s_blocks_count ||
            table_end > sb.s_blocks_count) return -1;
    }

    ext2_root = vfs_root;
    ext2_root->disk_inode = EXT2_ROOT_INO;
    struct ext2_inode root;
    if (inode_read(EXT2_ROOT_INO, &root) == 0)
        ext2_root->mode = root.i_mode & 07777U;

    ext2_loading = 1;
    int restored = load_directory(EXT2_ROOT_INO, ext2_root, 0);
    ext2_loading = 0;
    links_seen_reset();
    if (restored < 0) {
        kprintf("EXT2: root filesystem load failed\n");
        ext2_root->disk_inode = 0;
        return -1;
    }

    mark_volatile_dirs();
    ext2_mounted_flag = 1;
    vfs_set_persist_ops(&ext2_persist_ops);
    KDEBUG("EXT2: root loaded from disk, %d entries (%u/%u blocks free)\n",
           restored, (unsigned)sb.s_free_blocks_count,
           (unsigned)sb.s_blocks_count);
    return 0;
}

