#include "ext4.h"

#include <tunix/heap.h>
#include <tunix/kstring.h>
#include <tunix/random.h>

#define CHECKSUM_LO_AT 0x7CU
#define CHECKSUM_HI_AT 0x82U

uint8_t *inode_slot(uint32_t ino, uint8_t **table_out) {
    if (!ino || ino > fs->sb.s_inodes_count) return NULL;
    uint32_t index = ino - 1U;
    uint32_t group = index / fs->inodes_per_group;
    uint32_t within = index % fs->inodes_per_group;
    uint64_t block = gd_inode_table(group) + within / fs->inodes_per_block;
    uint8_t *table = buf_get(block);
    if (!table) return NULL;
    if (table_out) *table_out = table;
    return table + (within % fs->inodes_per_block) * fs->inode_size;
}

static uint32_t slot_extra(const uint8_t *slot) {
    if (fs->inode_size <= EXT4_GOOD_OLD_INODE_SIZE) return 0;
    uint32_t extra = get16(slot + EXT4_GOOD_OLD_INODE_SIZE);
    if (EXT4_GOOD_OLD_INODE_SIZE + extra > fs->inode_size || (extra & 3U)) return 0;
    return extra;
}

static uint32_t inode_csum(const uint8_t *slot, uint32_t ino) {
    uint16_t dummy = 0;
    uint32_t sum = ext4_inode_seed(ino, get32(slot + offsetof(struct ext4_inode, i_generation)));
    sum = ext4_csum(sum, slot, CHECKSUM_LO_AT);
    sum = ext4_csum(sum, &dummy, sizeof(dummy));
    sum =
        ext4_csum(sum, slot + CHECKSUM_LO_AT + 2U, EXT4_GOOD_OLD_INODE_SIZE - CHECKSUM_LO_AT - 2U);
    if (fs->inode_size > EXT4_GOOD_OLD_INODE_SIZE) {
        uint32_t at = CHECKSUM_HI_AT;
        sum = ext4_csum(sum, slot + EXT4_GOOD_OLD_INODE_SIZE, at - EXT4_GOOD_OLD_INODE_SIZE);
        if (EXT4_GOOD_OLD_INODE_SIZE + get16(slot + EXT4_GOOD_OLD_INODE_SIZE) >= at + 2U) {
            sum = ext4_csum(sum, &dummy, sizeof(dummy));
            at += 2U;
        }
        sum = ext4_csum(sum, slot + at, fs->inode_size - at);
    }
    return sum;
}

static void inode_csum_set(uint8_t *slot, uint32_t ino) {
    if (!has_csum()) return;
    uint32_t sum = inode_csum(slot, ino);
    put16(slot + CHECKSUM_LO_AT, (uint16_t)sum);
    if (fs->inode_size > EXT4_GOOD_OLD_INODE_SIZE &&
        EXT4_GOOD_OLD_INODE_SIZE + get16(slot + EXT4_GOOD_OLD_INODE_SIZE) >= CHECKSUM_HI_AT + 2U)
        put16(slot + CHECKSUM_HI_AT, (uint16_t)(sum >> 16));
}

static int inode_csum_valid(const uint8_t *slot, uint32_t ino) {
    if (!has_csum()) return 1;
    uint32_t sum = inode_csum(slot, ino);
    if (get16(slot + CHECKSUM_LO_AT) != (uint16_t)sum) return 0;
    if (fs->inode_size > EXT4_GOOD_OLD_INODE_SIZE &&
        EXT4_GOOD_OLD_INODE_SIZE + get16(slot + EXT4_GOOD_OLD_INODE_SIZE) >= CHECKSUM_HI_AT + 2U &&
        get16(slot + CHECKSUM_HI_AT) != (uint16_t)(sum >> 16))
        return 0;
    return 1;
}

int inode_read(uint32_t ino, struct ext4_inode *out) {
    uint8_t *slot = inode_slot(ino, NULL);
    if (!slot) return -1;
    memset(out, 0, sizeof(*out));
    uint32_t extra = slot_extra(slot);
    uint32_t bytes = EXT4_GOOD_OLD_INODE_SIZE + extra;
    if (bytes > sizeof(*out)) bytes = sizeof(*out);
    memcpy(out, slot, bytes);
    out->i_extra_isize = (uint16_t)extra;
    if (get16(slot) && !inode_csum_valid(slot, ino)) {
        static unsigned reported;
        if (reported++ < 8U)
            kprintf("EXT4: %s inode %u has a bad checksum\n", fs->device->dev_name, (unsigned)ino);
    }
    if (!extra && fs->extra_isize && !fs->read_only &&
        fs->inode_size >= EXT4_GOOD_OLD_INODE_SIZE + fs->extra_isize &&
        get32(slot + EXT4_GOOD_OLD_INODE_SIZE) != EXT4_XATTR_MAGIC)
        out->i_extra_isize = fs->extra_isize;
    return 0;
}

static uint32_t stored_bytes(const struct ext4_inode *in) {
    if (fs->inode_size <= EXT4_GOOD_OLD_INODE_SIZE) return EXT4_GOOD_OLD_INODE_SIZE;
    uint32_t bytes = EXT4_GOOD_OLD_INODE_SIZE + in->i_extra_isize;
    if (bytes > fs->inode_size) bytes = fs->inode_size;
    if (bytes > sizeof(*in)) bytes = sizeof(*in);
    return bytes;
}

int inode_write(uint32_t ino, const struct ext4_inode *in) {
    uint8_t *table = NULL;
    uint8_t *slot = inode_slot(ino, &table);
    if (!slot) return -1;
    uint32_t bytes = stored_bytes(in);
    if (fs->inode_size > EXT4_GOOD_OLD_INODE_SIZE) {
        uint32_t extra = slot_extra(slot);
        if (in->i_extra_isize != extra &&
            (extra || get32(slot + EXT4_GOOD_OLD_INODE_SIZE) == EXT4_XATTR_MAGIC))
            bytes = EXT4_GOOD_OLD_INODE_SIZE + extra;
    }
    memcpy(slot, in, bytes);
    if (fs->inode_size > EXT4_GOOD_OLD_INODE_SIZE && bytes == EXT4_GOOD_OLD_INODE_SIZE &&
        in->i_extra_isize == 0)
        put16(slot + EXT4_GOOD_OLD_INODE_SIZE, 0);
    inode_csum_set(slot, ino);
    buf_mark(table);
    return 0;
}

int inode_write_new(uint32_t ino, struct ext4_inode *in) {
    uint8_t *table = NULL;
    uint8_t *slot = inode_slot(ino, &table);
    if (!slot) return -1;
    memset(slot, 0, fs->inode_size);
    if (fs->inode_size > EXT4_GOOD_OLD_INODE_SIZE) in->i_extra_isize = fs->extra_isize;
    if (!in->i_generation) {
        uint32_t generation;
        random_get_bytes(&generation, sizeof(generation));
        in->i_generation = generation;
    }
    memcpy(slot, in, stored_bytes(in));
    inode_csum_set(slot, ino);
    buf_mark(table);
    return 0;
}

uint32_t inode_seed(uint32_t ino, const struct ext4_inode *inode) {
    return ext4_inode_seed(ino, inode->i_generation);
}

uint16_t inode_kind(const struct ext4_inode *inode) { return inode->i_mode & EXT4_S_IFMT; }

uint64_t inode_size_of(const struct ext4_inode *inode) {
    uint64_t size = inode->i_size_lo;
    if (fs->sb.s_rev_level >= 1 &&
        (inode_kind(inode) == EXT4_S_IFREG || has_incompat(EXT4_INCOMPAT_LARGEDIR) ||
         inode_kind(inode) == EXT4_S_IFDIR))
        size |= (uint64_t)inode->i_size_high << 32;
    return size;
}

void inode_set_size(struct ext4_inode *inode, uint64_t size) {
    if (fs->sb.s_rev_level < 1 && size > 0x7FFFFFFFULL) size = 0x7FFFFFFFULL;
    inode->i_size_lo = (uint32_t)size;
    if (fs->sb.s_rev_level < 1) return;
    inode->i_size_high = (uint32_t)(size >> 32);
    if (size > 0x7FFFFFFFULL && inode_kind(inode) == EXT4_S_IFREG)
        fs->sb.s_feature_ro_compat |= EXT4_RO_COMPAT_LARGE_FILE;
}

uint64_t inode_sectors(const struct ext4_inode *inode) {
    uint64_t value = inode->i_blocks_lo;
    if (has_ro_compat(EXT4_RO_COMPAT_HUGE_FILE)) {
        value |= (uint64_t)inode->i_blocks_high << 32;
        if (inode->i_flags & EXT4_HUGE_FILE_FL) value *= fs->sectors_per_block;
    }
    return value;
}

void inode_set_sectors(struct ext4_inode *inode, uint64_t sectors) {
    if (sectors <= 0xFFFFFFFFULL) {
        inode->i_blocks_lo = (uint32_t)sectors;
        inode->i_blocks_high = 0;
        inode->i_flags &= ~EXT4_HUGE_FILE_FL;
        return;
    }
    if (!has_ro_compat(EXT4_RO_COMPAT_HUGE_FILE))
        fs->sb.s_feature_ro_compat |= EXT4_RO_COMPAT_HUGE_FILE;
    if (sectors <= 0xFFFFFFFFFFFFULL) {
        inode->i_blocks_lo = (uint32_t)sectors;
        inode->i_blocks_high = (uint16_t)(sectors >> 32);
        inode->i_flags &= ~EXT4_HUGE_FILE_FL;
        return;
    }
    uint64_t blocks = sectors / fs->sectors_per_block;
    inode->i_blocks_lo = (uint32_t)blocks;
    inode->i_blocks_high = (uint16_t)(blocks >> 32);
    inode->i_flags |= EXT4_HUGE_FILE_FL;
}

void inode_count_block(struct ext4_inode *inode) {
    inode_set_sectors(inode, inode_sectors(inode) + fs->sectors_per_block);
}

void inode_uncount_block(struct ext4_inode *inode) {
    uint64_t sectors = inode_sectors(inode);
    inode_set_sectors(inode,
                      sectors >= fs->sectors_per_block ? sectors - fs->sectors_per_block : 0);
}

uint32_t inode_uid(const struct ext4_inode *inode) {
    return inode->i_uid | (uint32_t)inode->i_uid_high << 16;
}

uint32_t inode_gid(const struct ext4_inode *inode) {
    return inode->i_gid | (uint32_t)inode->i_gid_high << 16;
}

void inode_set_owner(struct ext4_inode *inode, uint32_t uid, uint32_t gid) {
    inode->i_uid = (uint16_t)uid;
    inode->i_uid_high = (uint16_t)(uid >> 16);
    inode->i_gid = (uint16_t)gid;
    inode->i_gid_high = (uint16_t)(gid >> 16);
}

uint64_t inode_file_acl(const struct ext4_inode *inode) {
    uint64_t block = inode->i_file_acl_lo;
    if (has_incompat(EXT4_INCOMPAT_64BIT)) block |= (uint64_t)inode->i_file_acl_high << 32;
    return block;
}

void inode_set_file_acl(struct ext4_inode *inode, uint64_t block) {
    inode->i_file_acl_lo = (uint32_t)block;
    inode->i_file_acl_high = has_incompat(EXT4_INCOMPAT_64BIT) ? (uint16_t)(block >> 32) : 0;
}

int inode_has_extra(const struct ext4_inode *inode, uint32_t field_end) {
    return fs->inode_size > EXT4_GOOD_OLD_INODE_SIZE &&
        EXT4_GOOD_OLD_INODE_SIZE + inode->i_extra_isize >= field_end;
}

#define FIELD_END(field) \
    (offsetof(struct ext4_inode, field) + sizeof(((struct ext4_inode *)0)->field))

static void decode_time(uint32_t field, uint32_t extra, int has_extra, uint32_t *seconds,
                        uint32_t *nanoseconds) {
    int64_t value = (int32_t)field;
    uint32_t nano = 0;
    if (has_extra) {
        value += (int64_t)(extra & 3U) << 32;
        nano = extra >> 2;
        if (nano > 999999999U) nano = 0;
    }
    if (value < 0) value = 0;
    if (value > 0xFFFFFFFFLL) value = 0xFFFFFFFFLL;
    *seconds = (uint32_t)value;
    *nanoseconds = nano;
}

static uint32_t encode_extra(uint32_t seconds, uint32_t nanoseconds) {
    int64_t full = (int64_t)(uint64_t)seconds;
    uint32_t epoch = (uint32_t)((full - (int64_t)(int32_t)seconds) >> 32) & 3U;
    return epoch | (nanoseconds << 2);
}

void inode_get_times(const struct ext4_inode *inode, struct vfs_node *node) {
    decode_time(inode->i_atime, inode->i_atime_extra,
                inode_has_extra(inode, FIELD_END(i_atime_extra)), &node->atime, &node->atime_ns);
    decode_time(inode->i_mtime, inode->i_mtime_extra,
                inode_has_extra(inode, FIELD_END(i_mtime_extra)), &node->mtime, &node->mtime_ns);
    decode_time(inode->i_ctime, inode->i_ctime_extra,
                inode_has_extra(inode, FIELD_END(i_ctime_extra)), &node->ctime, &node->ctime_ns);
    if (inode_has_extra(inode, FIELD_END(i_crtime_extra)) && inode->i_crtime)
        decode_time(inode->i_crtime, inode->i_crtime_extra, 1, &node->btime, &node->btime_ns);
    else node->btime = node->btime_ns = 0;
}

void inode_set_times(struct ext4_inode *inode, uint32_t atime, uint32_t atime_ns, uint32_t mtime,
                     uint32_t mtime_ns, uint32_t ctime, uint32_t ctime_ns) {
    inode->i_atime = atime;
    inode->i_mtime = mtime;
    inode->i_ctime = ctime;
    if (inode_has_extra(inode, FIELD_END(i_atime_extra)))
        inode->i_atime_extra = encode_extra(atime, atime_ns);
    if (inode_has_extra(inode, FIELD_END(i_mtime_extra)))
        inode->i_mtime_extra = encode_extra(mtime, mtime_ns);
    if (inode_has_extra(inode, FIELD_END(i_ctime_extra)))
        inode->i_ctime_extra = encode_extra(ctime, ctime_ns);
}

void inode_set_crtime(struct ext4_inode *inode, uint32_t seconds, uint32_t nanoseconds) {
    if (!inode_has_extra(inode, FIELD_END(i_crtime_extra))) return;
    inode->i_crtime = seconds;
    inode->i_crtime_extra = encode_extra(seconds, nanoseconds);
}

void inode_links_adjust(uint32_t ino, int delta) {
    struct ext4_inode inode;
    if (inode_read(ino, &inode) != 0) return;
    int directory = inode_kind(&inode) == EXT4_S_IFDIR;
    if (directory && inode.i_links_count == 1U) return;
    if (delta < 0 && inode.i_links_count) {
        inode.i_links_count--;
    } else if (delta > 0) {
        if (inode.i_links_count + 1U < EXT4_LINK_MAX) {
            inode.i_links_count++;
        } else if (directory) {
            inode.i_links_count = 1;
            fs->sb.s_feature_ro_compat |= EXT4_RO_COMPAT_DIR_NLINK;
        }
    }
    uint32_t seconds;
    uint32_t nanoseconds;
    ext4_now_ns(&seconds, &nanoseconds);
    inode.i_ctime = seconds;
    if (inode_has_extra(&inode, FIELD_END(i_ctime_extra)))
        inode.i_ctime_extra = encode_extra(seconds, nanoseconds);
    inode_write(ino, &inode);
}

int inode_is_fast_symlink(const struct ext4_inode *inode) {
    if (inode_kind(inode) != EXT4_S_IFLNK) return 0;
    if (inode->i_flags & EXT4_EA_INODE_FL)
        return inode_size_of(inode) && inode_size_of(inode) < EXT4_N_BLOCKS * 4U;
    if (inode->i_flags & EXT4_INLINE_DATA_FL) return 0;
    uint64_t xattr = inode_file_acl(inode) ? fs->sectors_per_block : 0;
    return inode_sectors(inode) == xattr;
}

static uint64_t ind_limit(void) {
    uint64_t pointers = fs->pointers;
    return EXT4_NDIR_BLOCKS + pointers + pointers * pointers + pointers * pointers * pointers;
}

int64_t ind_bmap(struct ext4_inode *inode, uint64_t lblk, int alloc, int *inode_dirty) {
    if (lblk >= ind_limit()) return alloc ? -1 : 0;
    if (lblk < EXT4_NDIR_BLOCKS) {
        uint32_t block = inode->i_block[lblk];
        if (block || !alloc) return block;
        uint64_t goal = lblk ? inode->i_block[lblk - 1U] + 1ULL : 0;
        uint64_t fresh = alloc_block(goal, 1);
        if (!fresh) return -1;
        inode->i_block[lblk] = (uint32_t)fresh;
        inode_count_block(inode);
        *inode_dirty = 1;
        return (int64_t)fresh;
    }

    uint64_t index = lblk - EXT4_NDIR_BLOCKS;
    uint64_t span = fs->pointers;
    uint32_t level = 1;
    while (index >= span) {
        index -= span;
        level++;
        span *= fs->pointers;
    }

    uint32_t top = EXT4_NDIR_BLOCKS - 1U + level;
    uint64_t block = inode->i_block[top];
    if (!block) {
        if (!alloc) return 0;
        block = alloc_block(inode->i_block[EXT4_NDIR_BLOCKS - 1U], 1);
        if (!block || !buf_zero(block)) return -1;
        inode->i_block[top] = (uint32_t)block;
        inode_count_block(inode);
        *inode_dirty = 1;
    }
    for (uint32_t depth = 0; depth < level; depth++) {
        span /= fs->pointers;
        uint32_t slot = (uint32_t)(index / span);
        index %= span;
        uint8_t *entries = buf_get(block);
        if (!entries) return -1;
        uint32_t child = get32(entries + slot * 4U);
        if (!child) {
            if (!alloc) return 0;
            uint64_t fresh = alloc_block(block + 1U, 1);
            if (!fresh) return -1;
            if (depth + 1U < level && !buf_zero(fresh)) return -1;
            entries = buf_get(block);
            if (!entries) return -1;
            put32(entries + slot * 4U, (uint32_t)fresh);
            buf_mark(entries);
            inode_count_block(inode);
            *inode_dirty = 1;
            child = (uint32_t)fresh;
        }
        block = child;
    }
    return (int64_t)block;
}

static void free_subtree(uint64_t block, uint32_t height, struct ext4_inode *inode) {
    uint8_t *entries = (uint8_t *)kmalloc(fs->block_size);
    if (entries && read_block(block, entries) == 0) {
        for (uint32_t slot = 0; slot < fs->pointers; slot++) {
            uint32_t child = get32(entries + slot * 4U);
            if (!child) continue;
            if (height > 1U) {
                free_subtree(child, height - 1U, inode);
            } else {
                free_block(child);
                inode_uncount_block(inode);
            }
        }
    }
    kfree(entries);
    free_block(block);
    inode_uncount_block(inode);
}

static int trim_subtree(uint64_t block, uint32_t height, uint64_t base, uint64_t keep,
                        struct ext4_inode *inode) {
    if (keep <= base) {
        free_subtree(block, height, inode);
        return 1;
    }
    uint64_t cover = 1;
    for (uint32_t level = 1; level < height; level++) cover *= fs->pointers;
    if (base + cover * fs->pointers <= keep) return 0;
    uint8_t *entries = (uint8_t *)kmalloc(fs->block_size);
    if (!entries) return 0;
    if (read_block(block, entries) != 0) {
        kfree(entries);
        return 0;
    }
    int changed = 0;
    for (uint32_t slot = 0; slot < fs->pointers; slot++) {
        uint32_t child = get32(entries + slot * 4U);
        if (!child) continue;
        uint64_t child_base = base + slot * cover;
        if (child_base + cover <= keep) continue;
        if (height == 1U) {
            free_block(child);
            inode_uncount_block(inode);
        } else if (!trim_subtree(child, height - 1U, child_base, keep, inode)) {
            continue;
        }
        put32(entries + slot * 4U, 0);
        changed = 1;
    }
    if (changed) buf_write(block, entries);
    kfree(entries);
    return 0;
}

void ind_truncate(struct ext4_inode *inode, uint64_t keep) {
    for (uint64_t index = keep; index < EXT4_NDIR_BLOCKS; index++) {
        if (!inode->i_block[index]) continue;
        free_block(inode->i_block[index]);
        inode_uncount_block(inode);
        inode->i_block[index] = 0;
    }
    uint64_t base = EXT4_NDIR_BLOCKS;
    uint64_t span = fs->pointers;
    for (uint32_t level = 1; level <= 3U; level++) {
        uint32_t top = EXT4_NDIR_BLOCKS - 1U + level;
        if (inode->i_block[top] && trim_subtree(inode->i_block[top], level, base, keep, inode))
            inode->i_block[top] = 0;
        base += span;
        span *= fs->pointers;
    }
}

int64_t inode_map(uint32_t ino, struct ext4_inode *inode, uint64_t lblk, uint32_t *run,
                  int *unwritten) {
    *run = 1;
    *unwritten = 0;
    if (inode->i_flags & EXT4_INLINE_DATA_FL) return 0;
    if (inode->i_flags & EXT4_EXTENTS_FL) return ext_map(ino, inode, lblk, run, unwritten);
    int dirty = 0;
    return ind_bmap(inode, lblk, 0, &dirty);
}

int64_t inode_bmap(uint32_t ino, struct ext4_inode *inode, uint64_t lblk, int alloc,
                   int *inode_dirty) {
    if (inode->i_flags & EXT4_INLINE_DATA_FL) {
        if (!alloc) return 0;
        if (inline_expand(ino, inode) != 0) return -1;
        *inode_dirty = 1;
    }
    if (inode->i_flags & EXT4_EXTENTS_FL) {
        if (alloc) return ext_alloc(ino, inode, lblk, inode_dirty);
        uint32_t run = 0;
        int unwritten = 0;
        int64_t block = ext_map(ino, inode, lblk, &run, &unwritten);
        return unwritten ? 0 : block;
    }
    return ind_bmap(inode, lblk, alloc, inode_dirty);
}

uint64_t inode_goal(uint32_t ino, struct ext4_inode *inode, uint64_t lblk) {
    if (lblk) {
        uint32_t run = 0;
        int unwritten = 0;
        int64_t before = inode_map(ino, inode, lblk - 1U, &run, &unwritten);
        if (before > 0) return (uint64_t)before + 1U;
    }
    uint32_t group = (ino - 1U) / fs->inodes_per_group;
    if (has_incompat(EXT4_INCOMPAT_FLEX_BG) && fs->sb.s_log_groups_per_flex < 32U) {
        uint32_t flex = 1U << fs->sb.s_log_groups_per_flex;
        group &= ~(flex - 1U);
        uint64_t table_end = gd_inode_table(group) + fs->inodes_per_group / fs->inodes_per_block;
        uint64_t candidate =
            table_end + (uint64_t)flex * fs->inodes_per_group / fs->inodes_per_block;
        if (candidate < fs->blocks_count) return candidate;
    }
    return group_first_block(group);
}

int inode_truncate_blocks(uint32_t ino, struct ext4_inode *inode, uint64_t keep) {
    if (inode->i_flags & EXT4_INLINE_DATA_FL) return 0;
    if (inode_is_fast_symlink(inode)) return 0;
    if (inode->i_flags & EXT4_EXTENTS_FL) return ext_truncate(ino, inode, keep);
    ind_truncate(inode, keep);
    return 0;
}

void inode_release_blocks(uint32_t ino, struct ext4_inode *inode) {
    (void)inode_truncate_blocks(ino, inode, 0);
}
