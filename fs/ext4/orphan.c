#include "ext4.h"

#include <tunix/heap.h>
#include <tunix/kstring.h>

#define TAIL_BYTES 8U

static uint32_t slots_per_block(void) { return (fs->block_size - TAIL_BYTES) / 4U; }

static uint8_t *orphan_block(uint32_t index, uint64_t *physical_out) {
    uint32_t run = 0;
    int unwritten = 0;
    int64_t physical = inode_map(fs->orphan_ino, &fs->orphan_inode, index, &run, &unwritten);
    if (physical <= 0) return NULL;
    *physical_out = (uint64_t)physical;
    return buf_get((uint64_t)physical);
}

static void orphan_block_seal(uint8_t *block, uint64_t physical) {
    if (!has_csum()) return;
    uint32_t sum =
        ext4_csum(inode_seed(fs->orphan_ino, &fs->orphan_inode), &physical, sizeof(physical));
    sum = ext4_csum(sum, block, slots_per_block() * 4U);
    put32(block + fs->block_size - 4U, sum);
}

int orphans_open(void) {
    fs->orphan_ino = 0;
    fs->orphan_blocks = 0;
    if (!has_compat(EXT4_COMPAT_ORPHAN_FILE) || !fs->sb.s_orphan_file_inum) return 0;
    uint32_t ino = fs->sb.s_orphan_file_inum;
    if (inode_read(ino, &fs->orphan_inode) != 0) return -1;
    uint64_t size = inode_size_of(&fs->orphan_inode);
    if (size % fs->block_size) return -1;
    fs->orphan_ino = ino;
    fs->orphan_blocks = (uint32_t)(size / fs->block_size);
    for (uint32_t index = 0; index < fs->orphan_blocks; index++) {
        uint64_t physical;
        uint8_t *block = orphan_block(index, &physical);
        if (!block || get32(block + fs->block_size - TAIL_BYTES) != EXT4_ORPHAN_BLOCK_MAGIC) {
            kprintf("EXT4: %s orphan file block %u is damaged\n", fs->device->dev_name,
                    (unsigned)index);
            fs->orphan_ino = 0;
            fs->orphan_blocks = 0;
            return 0;
        }
    }
    return 0;
}

static void settle_one(uint32_t ino, unsigned *freed, unsigned *trimmed) {
    struct ext4_inode inode;
    if (inode_read(ino, &inode) != 0 || !inode.i_mode) return;
    if (!inode.i_links_count) {
        int directory = inode_kind(&inode) == EXT4_S_IFDIR;
        if (!inode_is_fast_symlink(&inode)) inode_release_blocks(ino, &inode);
        xattr_release(ino, &inode);
        inode_set_size(&inode, 0);
        inode.i_dtime = ext4_now();
        inode_write(ino, &inode);
        free_inode(ino, directory);
        (*freed)++;
        return;
    }
    uint64_t keep = (inode_size_of(&inode) + fs->block_size - 1U) / fs->block_size;
    inode_truncate_blocks(ino, &inode, keep);
    inode.i_dtime = 0;
    inode_write(ino, &inode);
    (*trimmed)++;
}

int orphans_replay(void) {
    unsigned freed = 0;
    unsigned trimmed = 0;
    uint32_t next = fs->sb.s_last_orphan;
    uint32_t guard = fs->sb.s_inodes_count;
    while (next && guard--) {
        struct ext4_inode inode;
        if (next > fs->sb.s_inodes_count || inode_read(next, &inode) != 0) break;
        uint32_t following = inode.i_dtime;
        if (inode.i_links_count) {
            inode.i_dtime = 0;
            inode_write(next, &inode);
        }
        settle_one(next, &freed, &trimmed);
        next = following;
    }
    fs->sb.s_last_orphan = 0;
    for (uint32_t index = 0; index < fs->orphan_blocks; index++) {
        uint64_t physical;
        uint8_t *block = orphan_block(index, &physical);
        if (!block) continue;
        int changed = 0;
        for (uint32_t slot = 0; slot < slots_per_block(); slot++) {
            uint32_t ino = get32(block + slot * 4U);
            if (!ino) continue;
            if (ino <= fs->sb.s_inodes_count) settle_one(ino, &freed, &trimmed);
            block = orphan_block(index, &physical);
            if (!block) break;
            put32(block + slot * 4U, 0);
            changed = 1;
        }
        if (changed && block) {
            orphan_block_seal(block, physical);
            buf_mark(block);
        }
    }
    fs->sb.s_feature_ro_compat &= ~EXT4_RO_COMPAT_ORPHAN_PRESENT;
    if (freed || trimmed)
        kprintf("EXT4: %s had %u orphaned inodes freed and %u truncated\n", fs->device->dev_name,
                freed, trimmed);
    return 0;
}

int orphan_add(uint32_t ino) {
    for (uint32_t index = 0; index < fs->orphan_blocks; index++) {
        uint64_t physical;
        uint8_t *block = orphan_block(index, &physical);
        if (!block) continue;
        for (uint32_t slot = 0; slot < slots_per_block(); slot++) {
            if (get32(block + slot * 4U)) continue;
            put32(block + slot * 4U, ino);
            orphan_block_seal(block, physical);
            buf_mark(block);
            fs->sb.s_feature_ro_compat |= EXT4_RO_COMPAT_ORPHAN_PRESENT;
            return 0;
        }
    }
    struct ext4_inode inode;
    if (inode_read(ino, &inode) != 0) return -1;
    inode.i_dtime = fs->sb.s_last_orphan;
    if (inode_write(ino, &inode) != 0) return -1;
    fs->sb.s_last_orphan = ino;
    return 0;
}

int orphan_remove(uint32_t ino) {
    for (uint32_t index = 0; index < fs->orphan_blocks; index++) {
        uint64_t physical;
        uint8_t *block = orphan_block(index, &physical);
        if (!block) continue;
        for (uint32_t slot = 0; slot < slots_per_block(); slot++) {
            if (get32(block + slot * 4U) != ino) continue;
            put32(block + slot * 4U, 0);
            orphan_block_seal(block, physical);
            buf_mark(block);
            return 0;
        }
    }
    struct ext4_inode inode;
    if (inode_read(ino, &inode) != 0) return -1;
    uint32_t following = inode.i_dtime;
    if (fs->sb.s_last_orphan == ino) {
        fs->sb.s_last_orphan = following;
    } else {
        uint32_t walk = fs->sb.s_last_orphan;
        uint32_t guard = 4096;
        while (walk && guard--) {
            struct ext4_inode previous;
            if (walk > fs->sb.s_inodes_count || inode_read(walk, &previous) != 0) return -1;
            if (previous.i_dtime == ino) {
                previous.i_dtime = following;
                inode_write(walk, &previous);
                break;
            }
            walk = previous.i_dtime;
        }
        if (!walk) return -1;
    }
    inode.i_dtime = 0;
    return inode_write(ino, &inode);
}

void orphans_settle(void) {
    if (!(fs->sb.s_feature_ro_compat & EXT4_RO_COMPAT_ORPHAN_PRESENT)) return;
    for (uint32_t index = 0; index < fs->orphan_blocks; index++) {
        uint64_t physical;
        uint8_t *block = orphan_block(index, &physical);
        if (!block) return;
        for (uint32_t slot = 0; slot < slots_per_block(); slot++)
            if (get32(block + slot * 4U)) return;
    }
    fs->sb.s_feature_ro_compat &= ~EXT4_RO_COMPAT_ORPHAN_PRESENT;
}
