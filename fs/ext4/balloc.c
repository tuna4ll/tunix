#include "ext4.h"

#include <tunix/heap.h>
#include <tunix/kstring.h>

static int bit_test(const uint8_t *bits, uint32_t bit) {
    return (bits[bit >> 3] >> (bit & 7U)) & 1U;
}

static void bit_set(uint8_t *bits, uint32_t bit) { bits[bit >> 3] |= (uint8_t)(1U << (bit & 7U)); }

static void bit_clear(uint8_t *bits, uint32_t bit) {
    bits[bit >> 3] &= (uint8_t)~(1U << (bit & 7U));
}

static void bits_set_range(uint8_t *bits, uint32_t from, uint32_t to) {
    for (uint32_t bit = from; bit < to; bit++) bit_set(bits, bit);
}

static int64_t bitmap_scan(const uint8_t *bits, uint32_t start, uint32_t max_bits) {
    if (start >= max_bits) start = 0;
    for (int pass = 0; pass < 2; pass++) {
        uint32_t bit = pass ? 0 : start;
        uint32_t end = pass ? start : max_bits;
        while (bit < end) {
            if (!(bit & 31U) && bit + 32U <= end && get32(bits + (bit >> 3)) == 0xFFFFFFFFU) {
                bit += 32U;
                continue;
            }
            if (!bit_test(bits, bit)) return (int64_t)bit;
            bit++;
        }
    }
    return -1;
}

static int in_group(uint64_t block, uint32_t group) {
    uint64_t first = group_first_block(group);
    return block >= first && block < first + group_block_count(group);
}

static uint8_t *block_bitmap(uint32_t group) {
    struct ext4_group_desc *desc = gd(group);
    if (!has_group_csum() || !(desc->bg_flags & EXT4_BG_BLOCK_UNINIT))
        return buf_get(gd_block_bitmap(group));
    uint8_t *bits = buf_zero(gd_block_bitmap(group));
    if (!bits) return NULL;
    uint64_t first = group_first_block(group);
    bits_set_range(bits, 0, group_base_meta_blocks(group));
    uint64_t owned[2] = {gd_block_bitmap(group), gd_inode_bitmap(group)};
    for (int index = 0; index < 2; index++)
        if (in_group(owned[index], group)) bit_set(bits, (uint32_t)(owned[index] - first));
    uint64_t table = gd_inode_table(group);
    uint32_t table_blocks = fs->inodes_per_group / fs->inodes_per_block;
    for (uint64_t block = table; block < table + table_blocks; block++)
        if (in_group(block, group)) bit_set(bits, (uint32_t)(block - first));
    bits_set_range(bits, group_block_count(group), fs->block_size * 8U);
    desc->bg_flags &= (uint16_t)~EXT4_BG_BLOCK_UNINIT;
    fs->group_dirty[group] |= 1U | 4U;
    return bits;
}

static uint8_t *inode_bitmap(uint32_t group) {
    struct ext4_group_desc *desc = gd(group);
    if (!has_group_csum() || !(desc->bg_flags & EXT4_BG_INODE_UNINIT))
        return buf_get(gd_inode_bitmap(group));
    uint8_t *bits = buf_zero(gd_inode_bitmap(group));
    if (!bits) return NULL;
    bits_set_range(bits, fs->inodes_per_group, fs->block_size * 8U);
    desc->bg_flags &= (uint16_t)~EXT4_BG_INODE_UNINIT;
    fs->group_dirty[group] |= 2U | 4U;
    return bits;
}

uint64_t alloc_block(uint64_t goal, int below_4g) {
    uint32_t groups = fs->group_count;
    uint32_t start_group;
    uint32_t start_bit;
    if (goal >= fs->first_data_block && goal < fs->blocks_count) {
        start_group = (uint32_t)((goal - fs->first_data_block) / fs->blocks_per_group);
        start_bit = (uint32_t)((goal - fs->first_data_block) % fs->blocks_per_group);
    } else {
        if (fs->block_cursor_group >= groups) fs->block_cursor_group = fs->block_cursor_bit = 0;
        start_group = fs->block_cursor_group;
        start_bit = fs->block_cursor_bit;
    }
    for (uint32_t index = 0; index < groups; index++) {
        uint32_t group = start_group + index;
        if (group >= groups) group -= groups;
        if (!gd_free_blocks(group)) continue;
        uint64_t first = group_first_block(group);
        if (below_4g && first > 0xFFFFFFFFULL) continue;
        uint8_t *bits = block_bitmap(group);
        if (!bits) continue;
        uint32_t limit = group_block_count(group);
        if (below_4g && first + limit > 0x100000000ULL) limit = (uint32_t)(0x100000000ULL - first);
        int64_t bit = bitmap_scan(bits, index == 0 ? start_bit : 0, limit);
        if (bit < 0) continue;
        bit_set(bits, (uint32_t)bit);
        buf_mark(bits);
        gd_set_free_blocks(group, gd_free_blocks(group) - 1U);
        fs->group_dirty[group] |= 1U;
        uint64_t free_blocks = sb_free_blocks();
        if (free_blocks) sb_set_free_blocks(free_blocks - 1U);
        fs->block_cursor_group = group;
        fs->block_cursor_bit = (uint32_t)bit + 1U;
        return first + (uint64_t)bit;
    }
    kprintf("EXT4: %s is out of blocks\n", fs->device->dev_name);
    return 0;
}

void release_block_now(uint64_t block) {
    if (block < fs->first_data_block || block >= fs->blocks_count) return;
    uint64_t within = block - fs->first_data_block;
    uint32_t group = (uint32_t)(within / fs->blocks_per_group);
    uint8_t *bits = block_bitmap(group);
    if (!bits) return;
    uint32_t bit = (uint32_t)(within % fs->blocks_per_group);
    if (!bit_test(bits, bit)) {
        kprintf("EXT4: %s block %u freed twice\n", fs->device->dev_name, (unsigned)block);
        return;
    }
    bit_clear(bits, bit);
    buf_mark(bits);
    fs->group_dirty[group] |= 1U;
    sb_set_free_blocks(sb_free_blocks() + 1U);
    gd_set_free_blocks(group, gd_free_blocks(group) + 1U);
}

void apply_pending_frees(void) {
    for (uint32_t index = 0; index < fs->pending_free_count; index++)
        release_block_now(fs->pending_free[index]);
    fs->pending_free_count = 0;
}

void free_block(uint64_t block) {
    if (block < fs->first_data_block || block >= fs->blocks_count) return;
    buf_forget(block);
    if (fs->pending_free_count == fs->pending_free_capacity) {
        uint32_t capacity = fs->pending_free_capacity ? fs->pending_free_capacity * 2U : 256U;
        uint64_t *grown = (uint64_t *)kmalloc(capacity * sizeof(*grown));
        if (!grown) {
            release_block_now(block);
            return;
        }
        if (fs->pending_free_count)
            memcpy(grown, fs->pending_free, fs->pending_free_count * sizeof(*grown));
        kfree(fs->pending_free);
        fs->pending_free = grown;
        fs->pending_free_capacity = capacity;
    }
    fs->pending_free[fs->pending_free_count++] = block;
}

static uint32_t pick_directory_group(void) {
    uint32_t groups = fs->group_count;
    uint32_t average = fs->sb.s_free_inodes_count / groups;
    if (fs->inode_cursor_group >= groups) fs->inode_cursor_group = 0;
    for (uint32_t index = 0; index < groups; index++) {
        uint32_t group = fs->inode_cursor_group + index;
        if (group >= groups) group -= groups;
        if (gd_free_inodes(group) >= average && gd_free_inodes(group)) {
            fs->inode_cursor_group = group + 1U;
            return group;
        }
    }
    return 0;
}

uint32_t alloc_inode(uint32_t parent_ino, int directory) {
    uint32_t groups = fs->group_count;
    uint32_t start = 0;
    if (directory) start = pick_directory_group();
    else if (parent_ino && parent_ino <= fs->sb.s_inodes_count)
        start = (parent_ino - 1U) / fs->inodes_per_group;
    uint32_t first_ino = fs->sb.s_rev_level ? fs->sb.s_first_ino : 11U;
    for (uint32_t index = 0; index < groups; index++) {
        uint32_t group = start + index;
        if (group >= groups) group -= groups;
        if (!gd_free_inodes(group)) continue;
        uint8_t *bits = inode_bitmap(group);
        if (!bits) continue;
        int64_t found = -1;
        for (uint32_t from = 0; from < fs->inodes_per_group;) {
            int64_t bit = bitmap_scan(bits, from, fs->inodes_per_group);
            if (bit < 0 || (uint32_t)bit < from) break;
            uint32_t ino = group * fs->inodes_per_group + (uint32_t)bit + 1U;
            if (ino >= first_ino) {
                found = bit;
                break;
            }
            from = (uint32_t)bit + 1U;
        }
        if (found < 0) continue;
        bit_set(bits, (uint32_t)found);
        buf_mark(bits);
        fs->group_dirty[group] |= 2U;
        gd_set_free_inodes(group, gd_free_inodes(group) - 1U);
        if (fs->sb.s_free_inodes_count) fs->sb.s_free_inodes_count--;
        if (directory) gd_set_used_dirs(group, gd_used_dirs(group) + 1U);
        if (has_group_csum()) {
            uint32_t unused = gd_itable_unused(group);
            if (unused > fs->inodes_per_group) unused = fs->inodes_per_group;
            if ((uint32_t)found >= fs->inodes_per_group - unused)
                gd_set_itable_unused(group, fs->inodes_per_group - (uint32_t)found - 1U);
        }
        return group * fs->inodes_per_group + (uint32_t)found + 1U;
    }
    kprintf("EXT4: %s is out of inodes\n", fs->device->dev_name);
    return 0;
}

void free_inode(uint32_t ino, int is_directory) {
    if (!ino || ino > fs->sb.s_inodes_count) return;
    uint32_t index = ino - 1U;
    uint32_t group = index / fs->inodes_per_group;
    uint8_t *bits = inode_bitmap(group);
    if (!bits) return;
    uint32_t bit = index % fs->inodes_per_group;
    if (!bit_test(bits, bit)) {
        kprintf("EXT4: %s inode %u freed twice\n", fs->device->dev_name, (unsigned)ino);
        return;
    }
    bit_clear(bits, bit);
    buf_mark(bits);
    fs->group_dirty[group] |= 2U;
    fs->sb.s_free_inodes_count++;
    gd_set_free_inodes(group, gd_free_inodes(group) + 1U);
    if (is_directory && gd_used_dirs(group)) gd_set_used_dirs(group, gd_used_dirs(group) - 1U);
}

void inode_group_dirs_inc(uint32_t ino) {
    if (!ino || ino > fs->sb.s_inodes_count) return;
    uint32_t group = (ino - 1U) / fs->inodes_per_group;
    gd_set_used_dirs(group, gd_used_dirs(group) + 1U);
}
