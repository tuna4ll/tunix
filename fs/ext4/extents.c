#include "ext4.h"

#include <tunix/heap.h>
#include <tunix/kstring.h>

#define ROOT_MAX 4U

struct ext_level {
    uint8_t *node;
    uint64_t block;
    int index;
};

struct ext_path {
    int depth;
    struct ext_level level[EXT4_EXT_MAX_DEPTH + 1U];
};

static struct ext4_extent_header *header_of(uint8_t *node) {
    return (struct ext4_extent_header *)node;
}

static struct ext4_extent *extent_at(uint8_t *node, int index) {
    return (struct ext4_extent *)(node + sizeof(struct ext4_extent_header) +
                                  (size_t)index * sizeof(struct ext4_extent));
}

static struct ext4_extent_idx *index_at(uint8_t *node, int index) {
    return (struct ext4_extent_idx *)(node + sizeof(struct ext4_extent_header) +
                                      (size_t)index * sizeof(struct ext4_extent_idx));
}

static uint64_t extent_start(const struct ext4_extent *extent) {
    return extent->ee_start_lo | (uint64_t)extent->ee_start_hi << 32;
}

static void extent_set_start(struct ext4_extent *extent, uint64_t block) {
    extent->ee_start_lo = (uint32_t)block;
    extent->ee_start_hi = (uint16_t)(block >> 32);
}

static uint32_t extent_length(const struct ext4_extent *extent) {
    return extent->ee_len > EXT4_EXT_INIT_MAX ? extent->ee_len - EXT4_EXT_INIT_MAX : extent->ee_len;
}

static int extent_unwritten(const struct ext4_extent *extent) {
    return extent->ee_len > EXT4_EXT_INIT_MAX;
}

static void extent_set_length(struct ext4_extent *extent, uint32_t length, int unwritten) {
    extent->ee_len = (uint16_t)(unwritten ? length + EXT4_EXT_INIT_MAX : length);
}

static uint32_t extent_limit(int unwritten) {
    return unwritten ? EXT4_EXT_UNINIT_MAX : EXT4_EXT_INIT_MAX;
}

static uint64_t index_leaf(const struct ext4_extent_idx *index) {
    return index->ei_leaf_lo | (uint64_t)index->ei_leaf_hi << 32;
}

static void index_set_leaf(struct ext4_extent_idx *index, uint64_t block) {
    index->ei_leaf_lo = (uint32_t)block;
    index->ei_leaf_hi = (uint16_t)(block >> 32);
    index->ei_unused = 0;
}

static uint16_t block_max(void) {
    return (uint16_t)((fs->block_size - sizeof(struct ext4_extent_header)) /
                      sizeof(struct ext4_extent));
}

static uint32_t tail_offset(uint8_t *node) {
    return (uint32_t)sizeof(struct ext4_extent_header) +
        (uint32_t)header_of(node)->eh_max * (uint32_t)sizeof(struct ext4_extent);
}

void ext_block_seal(uint32_t seed, uint8_t *block) {
    if (!has_csum()) return;
    uint32_t at = tail_offset(block);
    if (at + 4U > fs->block_size) return;
    put32(block + at, ext4_csum(seed, block, at));
}

void ext_init_root(struct ext4_inode *inode) {
    memset(inode->i_block, 0, sizeof(inode->i_block));
    struct ext4_extent_header *root = (struct ext4_extent_header *)inode->i_block;
    root->eh_magic = EXT4_EXT_MAGIC;
    root->eh_entries = 0;
    root->eh_max = ROOT_MAX;
    root->eh_depth = 0;
    root->eh_generation = 0;
    inode->i_flags |= EXT4_EXTENTS_FL;
}

static int header_valid(uint8_t *node, int is_root, uint32_t depth) {
    struct ext4_extent_header *header = header_of(node);
    if (header->eh_magic != EXT4_EXT_MAGIC) return 0;
    if (header->eh_entries > header->eh_max) return 0;
    if (is_root) return header->eh_max <= ROOT_MAX && header->eh_depth <= EXT4_EXT_MAX_DEPTH;
    return header->eh_depth == depth && header->eh_max <= block_max() && header->eh_max;
}

static void report_tree(uint32_t ino) {
    static unsigned reported;
    if (reported++ < 8U)
        kprintf("EXT4: %s inode %u has a damaged extent tree\n", fs->device->dev_name,
                (unsigned)ino);
}

static int find_path(uint32_t ino, struct ext4_inode *inode, uint64_t lblk, struct ext_path *path) {
    uint8_t *node = (uint8_t *)inode->i_block;
    if (!header_valid(node, 1, 0)) {
        report_tree(ino);
        return -1;
    }
    int depth = header_of(node)->eh_depth;
    path->depth = depth;
    path->level[0].node = node;
    path->level[0].block = 0;
    for (int level = 0;; level++) {
        struct ext4_extent_header *header = header_of(node);
        int count = header->eh_entries;
        int low = 0;
        int high = count - 1;
        int found = -1;
        while (low <= high) {
            int middle = low + (high - low) / 2;
            uint32_t key = level < depth ? index_at(node, middle)->ei_block
                                         : extent_at(node, middle)->ee_block;
            if (key <= lblk) {
                found = middle;
                low = middle + 1;
            } else {
                high = middle - 1;
            }
        }
        if (level == depth) {
            path->level[level].index = found;
            return 0;
        }
        if (!count) {
            report_tree(ino);
            return -1;
        }
        if (found < 0) found = 0;
        path->level[level].index = found;
        uint64_t child = index_leaf(index_at(node, found));
        node = buf_get(child);
        if (!node || !header_valid(node, 0, (uint32_t)(depth - level - 1))) {
            report_tree(ino);
            return -1;
        }
        path->level[level + 1].node = node;
        path->level[level + 1].block = child;
    }
}

static void level_dirty(uint32_t seed, struct ext_level *level, int *inode_dirty) {
    if (!level->block) {
        *inode_dirty = 1;
        return;
    }
    ext_block_seal(seed, level->node);
    buf_mark(level->node);
}

int64_t ext_map(uint32_t ino, struct ext4_inode *inode, uint64_t lblk, uint32_t *run,
                int *unwritten) {
    *run = 1;
    *unwritten = 0;
    if (lblk > 0xFFFFFFFFULL) return 0;
    struct ext_path path;
    if (find_path(ino, inode, lblk, &path) != 0) return -1;
    struct ext_level *leaf = &path.level[path.depth];
    if (leaf->index < 0) return 0;
    struct ext4_extent *extent = extent_at(leaf->node, leaf->index);
    uint32_t length = extent_length(extent);
    if (lblk >= (uint64_t)extent->ee_block + length) return 0;
    uint64_t offset = lblk - extent->ee_block;
    *run = (uint32_t)(length - offset);
    *unwritten = extent_unwritten(extent);
    return (int64_t)(extent_start(extent) + offset);
}

static void correct_keys(uint32_t seed, struct ext_path *path, int level, int *inode_dirty) {
    uint8_t *node = path->level[level].node;
    if (!header_of(node)->eh_entries) return;
    uint32_t first =
        level == path->depth ? extent_at(node, 0)->ee_block : index_at(node, 0)->ei_block;
    for (int above = level - 1; above >= 0; above--) {
        struct ext_level *parent = &path->level[above];
        struct ext4_extent_idx *entry = index_at(parent->node, parent->index);
        if (entry->ei_block <= first) return;
        entry->ei_block = first;
        level_dirty(seed, parent, inode_dirty);
        if (parent->index != 0) return;
    }
}

static int grow_depth(uint32_t ino, struct ext4_inode *inode, int *inode_dirty) {
    uint8_t *root = (uint8_t *)inode->i_block;
    struct ext4_extent_header *header = header_of(root);
    if (header->eh_depth >= EXT4_EXT_MAX_DEPTH) return -1;
    uint64_t goal = inode_goal(ino, inode, 0);
    uint64_t block = alloc_block(goal, 0);
    if (!block) return -1;
    uint8_t *node = buf_zero(block);
    if (!node) {
        free_block(block);
        return -1;
    }
    memcpy(node, root, sizeof(inode->i_block));
    header_of(node)->eh_max = block_max();
    uint32_t first = header->eh_depth ? index_at(root, 0)->ei_block
                                      : (header->eh_entries ? extent_at(root, 0)->ee_block : 0);
    ext_block_seal(inode_seed(ino, inode), node);
    inode_count_block(inode);
    memset(root + sizeof(*header), 0, sizeof(inode->i_block) - sizeof(*header));
    header->eh_depth++;
    header->eh_entries = 1;
    struct ext4_extent_idx *entry = index_at(root, 0);
    entry->ei_block = first;
    index_set_leaf(entry, block);
    *inode_dirty = 1;
    return 0;
}

static int split_level(uint32_t ino, struct ext4_inode *inode, struct ext_path *path, int level,
                       uint64_t lblk, int *inode_dirty) {
    uint32_t seed = inode_seed(ino, inode);
    struct ext_level *child = &path->level[level];
    struct ext_level *parent = &path->level[level - 1];
    struct ext4_extent_header *header = header_of(child->node);
    int count = header->eh_entries;
    int is_leaf = level == path->depth;
    int move_from;
    uint32_t border;
    if (is_leaf) {
        move_from = child->index + 1;
        if (move_from == 0) move_from = count / 2;
    } else {
        move_from = count / 2;
    }
    if (move_from < count) {
        border = is_leaf ? extent_at(child->node, move_from)->ee_block
                         : index_at(child->node, move_from)->ei_block;
    } else {
        border = (uint32_t)lblk;
    }

    uint64_t block = alloc_block(child->block + 1U, 0);
    if (!block) return -1;
    uint8_t *node = buf_zero(block);
    if (!node) {
        free_block(block);
        return -1;
    }
    child->node = buf_get(child->block);
    parent->node = parent->block ? buf_get(parent->block) : parent->node;
    if (!child->node || !parent->node) return -1;
    header = header_of(child->node);
    struct ext4_extent_header *fresh = header_of(node);
    fresh->eh_magic = EXT4_EXT_MAGIC;
    fresh->eh_max = block_max();
    fresh->eh_depth = header->eh_depth;
    fresh->eh_entries = (uint16_t)(count - move_from);
    if (count > move_from)
        memcpy(node + sizeof(*fresh),
               child->node + sizeof(*header) + (size_t)move_from * sizeof(struct ext4_extent),
               (size_t)(count - move_from) * sizeof(struct ext4_extent));
    header->eh_entries = (uint16_t)move_from;
    memset(child->node + sizeof(*header) + (size_t)move_from * sizeof(struct ext4_extent), 0,
           (size_t)(count - move_from) * sizeof(struct ext4_extent));
    ext_block_seal(seed, node);
    ext_block_seal(seed, child->node);
    buf_mark(child->node);
    inode_count_block(inode);
    *inode_dirty = 1;

    struct ext4_extent_header *above = header_of(parent->node);
    int at = parent->index + 1;
    memmove(parent->node + sizeof(*above) + (size_t)(at + 1) * sizeof(struct ext4_extent_idx),
            parent->node + sizeof(*above) + (size_t)at * sizeof(struct ext4_extent_idx),
            (size_t)(above->eh_entries - at) * sizeof(struct ext4_extent_idx));
    struct ext4_extent_idx *entry = index_at(parent->node, at);
    entry->ei_block = border;
    index_set_leaf(entry, block);
    above->eh_entries++;
    level_dirty(seed, parent, inode_dirty);
    return 0;
}

static int make_room(uint32_t ino, struct ext4_inode *inode, uint64_t lblk, int *inode_dirty) {
    struct ext_path path;
    if (find_path(ino, inode, lblk, &path) != 0) return -1;
    int level = path.depth;
    while (level >= 0) {
        struct ext4_extent_header *header = header_of(path.level[level].node);
        if (header->eh_entries < header->eh_max) break;
        level--;
    }
    if (level < 0) return grow_depth(ino, inode, inode_dirty);
    if (level == path.depth) return 0;
    return split_level(ino, inode, &path, level + 1, lblk, inode_dirty);
}

static int insert_extent(uint32_t ino, struct ext4_inode *inode, uint64_t lblk, uint64_t pblk,
                         uint32_t length, int unwritten, int *inode_dirty) {
    uint32_t seed = inode_seed(ino, inode);
    for (int attempt = 0; attempt < 16; attempt++) {
        struct ext_path path;
        if (find_path(ino, inode, lblk, &path) != 0) return -1;
        struct ext_level *leaf = &path.level[path.depth];
        struct ext4_extent_header *header = header_of(leaf->node);
        int at = leaf->index;
        uint32_t limit = extent_limit(unwritten);

        if (at >= 0) {
            struct ext4_extent *left = extent_at(leaf->node, at);
            uint32_t left_length = extent_length(left);
            if ((uint64_t)left->ee_block + left_length == lblk &&
                extent_start(left) + left_length == pblk && extent_unwritten(left) == unwritten &&
                left_length + length <= limit) {
                extent_set_length(left, left_length + length, unwritten);
                if (at + 1 < header->eh_entries) {
                    struct ext4_extent *right = extent_at(leaf->node, at + 1);
                    uint32_t joined = extent_length(left) + extent_length(right);
                    if ((uint64_t)left->ee_block + extent_length(left) == right->ee_block &&
                        extent_start(left) + extent_length(left) == extent_start(right) &&
                        extent_unwritten(right) == unwritten && joined <= limit) {
                        extent_set_length(left, joined, unwritten);
                        memmove(right, right + 1,
                                (size_t)(header->eh_entries - at - 2) * sizeof(*right));
                        header->eh_entries--;
                        memset(extent_at(leaf->node, header->eh_entries), 0, sizeof(*right));
                    }
                }
                level_dirty(seed, leaf, inode_dirty);
                return 0;
            }
        }
        if (at + 1 < header->eh_entries) {
            struct ext4_extent *right = extent_at(leaf->node, at + 1);
            uint32_t right_length = extent_length(right);
            if (lblk + length == right->ee_block && pblk + length == extent_start(right) &&
                extent_unwritten(right) == unwritten && right_length + length <= limit) {
                right->ee_block = (uint32_t)lblk;
                extent_set_start(right, pblk);
                extent_set_length(right, right_length + length, unwritten);
                level_dirty(seed, leaf, inode_dirty);
                if (at + 1 == 0) correct_keys(seed, &path, path.depth, inode_dirty);
                return 0;
            }
        }
        if (header->eh_entries < header->eh_max) {
            int slot = at + 1;
            memmove(extent_at(leaf->node, slot + 1), extent_at(leaf->node, slot),
                    (size_t)(header->eh_entries - slot) * sizeof(struct ext4_extent));
            struct ext4_extent *fresh = extent_at(leaf->node, slot);
            fresh->ee_block = (uint32_t)lblk;
            extent_set_start(fresh, pblk);
            extent_set_length(fresh, length, unwritten);
            header->eh_entries++;
            level_dirty(seed, leaf, inode_dirty);
            if (slot == 0) correct_keys(seed, &path, path.depth, inode_dirty);
            return 0;
        }
        if (make_room(ino, inode, lblk, inode_dirty) != 0) return -1;
    }
    return -1;
}

static void remove_leaf_entry(uint32_t seed, struct ext_path *path, int *inode_dirty) {
    struct ext_level *leaf = &path->level[path->depth];
    struct ext4_extent_header *header = header_of(leaf->node);
    int at = leaf->index;
    memmove(extent_at(leaf->node, at), extent_at(leaf->node, at + 1),
            (size_t)(header->eh_entries - at - 1) * sizeof(struct ext4_extent));
    header->eh_entries--;
    memset(extent_at(leaf->node, header->eh_entries), 0, sizeof(struct ext4_extent));
    level_dirty(seed, leaf, inode_dirty);
    if (at == 0) correct_keys(seed, path, path->depth, inode_dirty);
}

static int convert_unwritten(uint32_t ino, struct ext4_inode *inode, struct ext_path *path,
                             uint64_t lblk, int *inode_dirty) {
    uint32_t seed = inode_seed(ino, inode);
    struct ext_level *leaf = &path->level[path->depth];
    struct ext4_extent *extent = extent_at(leaf->node, leaf->index);
    uint64_t start = extent->ee_block;
    uint32_t length = extent_length(extent);
    uint64_t physical = extent_start(extent);
    uint64_t target = physical + (lblk - start);
    uint64_t after = start + length - lblk - 1U;

    if (lblk > start) {
        extent_set_length(extent, (uint32_t)(lblk - start), 1);
        level_dirty(seed, leaf, inode_dirty);
    } else {
        remove_leaf_entry(seed, path, inode_dirty);
    }
    if (insert_extent(ino, inode, lblk, target, 1, 0, inode_dirty) != 0) return -1;
    if (after &&
        insert_extent(ino, inode, lblk + 1U, target + 1U, (uint32_t)after, 1, inode_dirty) != 0)
        return -1;
    return 0;
}

int64_t ext_alloc(uint32_t ino, struct ext4_inode *inode, uint64_t lblk, int *inode_dirty) {
    if (lblk > 0xFFFFFFFEULL) return -1;
    struct ext_path path;
    if (find_path(ino, inode, lblk, &path) != 0) return -1;
    struct ext_level *leaf = &path.level[path.depth];
    uint64_t goal = 0;
    if (leaf->index >= 0) {
        struct ext4_extent *extent = extent_at(leaf->node, leaf->index);
        uint32_t length = extent_length(extent);
        if (lblk < (uint64_t)extent->ee_block + length) {
            uint64_t physical = extent_start(extent) + (lblk - extent->ee_block);
            if (!extent_unwritten(extent)) return (int64_t)physical;
            if (convert_unwritten(ino, inode, &path, lblk, inode_dirty) != 0) return -1;
            return (int64_t)physical;
        }
        goal = extent_start(extent) + (lblk - extent->ee_block);
    } else if (header_of(leaf->node)->eh_entries) {
        struct ext4_extent *right = extent_at(leaf->node, 0);
        uint64_t distance = right->ee_block - lblk;
        if (extent_start(right) > distance) goal = extent_start(right) - distance;
    }
    if (!goal) goal = inode_goal(ino, inode, lblk);
    uint64_t block = alloc_block(goal, 0);
    if (!block) return -1;
    if (insert_extent(ino, inode, lblk, block, 1, 0, inode_dirty) != 0) {
        free_block(block);
        return -1;
    }
    inode_count_block(inode);
    *inode_dirty = 1;
    return (int64_t)block;
}

static void free_run(struct ext4_inode *inode, uint64_t start, uint64_t count) {
    for (uint64_t index = 0; index < count; index++) {
        free_block(start + index);
        inode_uncount_block(inode);
    }
}

static void free_subtree(struct ext4_inode *inode, uint64_t block, uint32_t depth) {
    uint8_t *copy = (uint8_t *)kmalloc(fs->block_size);
    if (copy && read_block(block, copy) == 0 && header_valid(copy, 0, depth)) {
        struct ext4_extent_header *header = header_of(copy);
        for (int index = 0; index < header->eh_entries; index++) {
            if (depth) {
                free_subtree(inode, index_leaf(index_at(copy, index)), depth - 1U);
            } else {
                struct ext4_extent *extent = extent_at(copy, index);
                free_run(inode, extent_start(extent), extent_length(extent));
            }
        }
    }
    kfree(copy);
    free_block(block);
    inode_uncount_block(inode);
}

static int trim_node(uint32_t seed, struct ext4_inode *inode, uint8_t *node, uint64_t block,
                     uint64_t keep, int *inode_dirty) {
    struct ext4_extent_header *header = header_of(node);
    int changed = 0;
    if (!header->eh_depth) {
        for (int index = header->eh_entries - 1; index >= 0; index--) {
            struct ext4_extent *extent = extent_at(node, index);
            uint64_t start = extent->ee_block;
            uint32_t length = extent_length(extent);
            if (start + length <= keep) break;
            if (start >= keep) {
                free_run(inode, extent_start(extent), length);
                memmove(extent, extent + 1,
                        (size_t)(header->eh_entries - index - 1) * sizeof(*extent));
                header->eh_entries--;
                memset(extent_at(node, header->eh_entries), 0, sizeof(*extent));
            } else {
                uint32_t kept = (uint32_t)(keep - start);
                free_run(inode, extent_start(extent) + kept, length - kept);
                extent_set_length(extent, kept, extent_unwritten(extent));
            }
            changed = 1;
        }
    } else {
        for (int index = header->eh_entries - 1; index >= 0; index--) {
            struct ext4_extent_idx *entry = index_at(node, index);
            uint64_t child = index_leaf(entry);
            int remove = 0;
            if (entry->ei_block >= keep) {
                free_subtree(inode, child, header->eh_depth - 1U);
                remove = 1;
            } else {
                uint8_t *data = buf_get(child);
                if (data && header_valid(data, 0, header->eh_depth - 1U)) {
                    int empty = trim_node(seed, inode, data, child, keep, inode_dirty);
                    if (empty) {
                        free_block(child);
                        inode_uncount_block(inode);
                        remove = 1;
                    }
                }
                data = block ? buf_get(block) : node;
                if (data) node = data;
                header = header_of(node);
                entry = index_at(node, index);
            }
            if (remove) {
                memmove(entry, entry + 1,
                        (size_t)(header->eh_entries - index - 1) * sizeof(*entry));
                header->eh_entries--;
                memset(index_at(node, header->eh_entries), 0, sizeof(*entry));
                changed = 1;
            }
            if (!remove) break;
        }
    }
    if (changed) {
        if (block) {
            ext_block_seal(seed, node);
            buf_mark(node);
        } else {
            *inode_dirty = 1;
        }
    }
    return header->eh_entries == 0;
}

int ext_truncate(uint32_t ino, struct ext4_inode *inode, uint64_t keep) {
    uint8_t *root = (uint8_t *)inode->i_block;
    if (!header_valid(root, 1, 0)) {
        report_tree(ino);
        return -1;
    }
    int dirty = 0;
    int empty = trim_node(inode_seed(ino, inode), inode, root, 0, keep, &dirty);
    if (empty && header_of(root)->eh_depth) {
        uint32_t flags = inode->i_flags;
        ext_init_root(inode);
        inode->i_flags = flags | EXT4_EXTENTS_FL;
    }
    return 0;
}
