#include "ext4.h"

#include <tunix/heap.h>
#include <tunix/kstring.h>

#define TAIL_BYTES     12U
#define DX_INFO_AT     24U
#define DX_NODE_OFFSET 8U
#define DX_ENTRY_BYTES 8U
#define DX_TAIL_BYTES  8U
#define DX_BLOCK_MASK  0x0FFFFFFFU
#define DX_MAX_LEVELS  3

static uint32_t rotate_left(uint32_t value, unsigned shift) {
    return (value << shift) | (value >> (32U - shift));
}

static void tea_transform(uint32_t buf[4], const uint32_t in[4]) {
    uint32_t sum = 0;
    uint32_t b0 = buf[0];
    uint32_t b1 = buf[1];
    uint32_t a = in[0], b = in[1], c = in[2], d = in[3];
    for (int round = 0; round < 16; round++) {
        sum += 0x9E3779B9U;
        b0 += ((b1 << 4) + a) ^ (b1 + sum) ^ ((b1 >> 5) + b);
        b1 += ((b0 << 4) + c) ^ (b0 + sum) ^ ((b0 >> 5) + d);
    }
    buf[0] += b0;
    buf[1] += b1;
}

#define MD4_F(x, y, z)                 ((z) ^ ((x) & ((y) ^ (z))))
#define MD4_G(x, y, z)                 (((x) & (y)) + (((x) ^ (y)) & (z)))
#define MD4_H(x, y, z)                 ((x) ^ (y) ^ (z))
#define MD4_ROUND(f, a, b, c, d, x, s) ((a) += f((b), (c), (d)) + (x), (a) = rotate_left((a), (s)))
#define MD4_K2                         013240474631U
#define MD4_K3                         015666365641U

static void half_md4_transform(uint32_t buf[4], const uint32_t in[8]) {
    uint32_t a = buf[0], b = buf[1], c = buf[2], d = buf[3];
    MD4_ROUND(MD4_F, a, b, c, d, in[0], 3);
    MD4_ROUND(MD4_F, d, a, b, c, in[1], 7);
    MD4_ROUND(MD4_F, c, d, a, b, in[2], 11);
    MD4_ROUND(MD4_F, b, c, d, a, in[3], 19);
    MD4_ROUND(MD4_F, a, b, c, d, in[4], 3);
    MD4_ROUND(MD4_F, d, a, b, c, in[5], 7);
    MD4_ROUND(MD4_F, c, d, a, b, in[6], 11);
    MD4_ROUND(MD4_F, b, c, d, a, in[7], 19);
    MD4_ROUND(MD4_G, a, b, c, d, in[1] + MD4_K2, 3);
    MD4_ROUND(MD4_G, d, a, b, c, in[3] + MD4_K2, 5);
    MD4_ROUND(MD4_G, c, d, a, b, in[5] + MD4_K2, 9);
    MD4_ROUND(MD4_G, b, c, d, a, in[7] + MD4_K2, 13);
    MD4_ROUND(MD4_G, a, b, c, d, in[0] + MD4_K2, 3);
    MD4_ROUND(MD4_G, d, a, b, c, in[2] + MD4_K2, 5);
    MD4_ROUND(MD4_G, c, d, a, b, in[4] + MD4_K2, 9);
    MD4_ROUND(MD4_G, b, c, d, a, in[6] + MD4_K2, 13);
    MD4_ROUND(MD4_H, a, b, c, d, in[3] + MD4_K3, 3);
    MD4_ROUND(MD4_H, d, a, b, c, in[7] + MD4_K3, 9);
    MD4_ROUND(MD4_H, c, d, a, b, in[2] + MD4_K3, 11);
    MD4_ROUND(MD4_H, b, c, d, a, in[6] + MD4_K3, 15);
    MD4_ROUND(MD4_H, a, b, c, d, in[1] + MD4_K3, 3);
    MD4_ROUND(MD4_H, d, a, b, c, in[5] + MD4_K3, 9);
    MD4_ROUND(MD4_H, c, d, a, b, in[0] + MD4_K3, 11);
    MD4_ROUND(MD4_H, b, c, d, a, in[4] + MD4_K3, 15);
    buf[0] += a;
    buf[1] += b;
    buf[2] += c;
    buf[3] += d;
}

static uint32_t legacy_hash(const char *name, size_t length, int is_signed) {
    uint32_t hash0 = 0x12A3FE2DU;
    uint32_t hash1 = 0x37ABE8F9U;
    for (size_t index = 0; index < length; index++) {
        int value = is_signed ? (int)(signed char)name[index] : (int)(unsigned char)name[index];
        uint32_t hash = hash1 + (hash0 ^ ((uint32_t)value * 7152373U));
        if (hash & 0x80000000U) hash -= 0x7FFFFFFFU;
        hash1 = hash0;
        hash0 = hash;
    }
    return hash0 << 1;
}

static void str_to_hashbuf(const char *message, size_t length, uint32_t *buf, int words,
                           int is_signed) {
    uint32_t pad = (uint32_t)length | ((uint32_t)length << 8);
    pad |= pad << 16;
    uint32_t value = pad;
    if (length > (size_t)words * 4U) length = (size_t)words * 4U;
    for (size_t index = 0; index < length; index++) {
        int byte =
            is_signed ? (int)(signed char)message[index] : (int)(unsigned char)message[index];
        value = (uint32_t)byte + (value << 8);
        if ((index % 4U) == 3U) {
            *buf++ = value;
            value = pad;
            words--;
        }
    }
    if (--words >= 0) *buf++ = value;
    while (--words >= 0) *buf++ = pad;
}

uint32_t ext4_dirhash(const char *name, size_t length, uint32_t version, const uint32_t seed[4],
                      uint32_t *minor) {
    uint32_t buf[4] = {0x67452301U, 0xEFCDAB89U, 0x98BADCFEU, 0x10325476U};
    if (seed && (seed[0] | seed[1] | seed[2] | seed[3])) memcpy(buf, seed, sizeof(buf));
    uint32_t hash = 0;
    uint32_t minor_hash = 0;
    uint32_t input[8];
    int is_signed = version <= DX_HASH_TEA;
    switch (version) {
    case DX_HASH_LEGACY:
    case DX_HASH_LEGACY_UNSIGNED:   hash = legacy_hash(name, length, is_signed); break;
    case DX_HASH_HALF_MD4:
    case DX_HASH_HALF_MD4_UNSIGNED: {
        const char *at = name;
        int64_t left = (int64_t)length;
        while (left > 0) {
            str_to_hashbuf(at, (size_t)left, input, 8, is_signed);
            half_md4_transform(buf, input);
            left -= 32;
            at += 32;
        }
        minor_hash = buf[2];
        hash = buf[1];
        break;
    }
    case DX_HASH_TEA:
    case DX_HASH_TEA_UNSIGNED: {
        const char *at = name;
        int64_t left = (int64_t)length;
        while (left > 0) {
            str_to_hashbuf(at, (size_t)left, input, 4, is_signed);
            tea_transform(buf, input);
            left -= 16;
            at += 16;
        }
        hash = buf[0];
        minor_hash = buf[1];
        break;
    }
    default: hash = 0; break;
    }
    hash &= ~1U;
    if (hash == (0x7FFFFFFFU << 1)) hash = (0x7FFFFFFFU - 1U) << 1;
    if (minor) *minor = minor_hash;
    return hash;
}

uint32_t dirent_rec_len(uint32_t name_len) { return (8U + name_len + 3U) & ~3U; }

int dirent_sane(const struct ext4_dir_entry *entry, uint32_t at, uint32_t size) {
    return entry->rec_len >= 8U && !(entry->rec_len & 3U) && at + entry->rec_len <= size &&
        8U + (uint32_t)entry->name_len <= entry->rec_len;
}

int dir_tail_present(const uint8_t *block) {
    const uint8_t *tail = block + fs->block_size - TAIL_BYTES;
    return get32(tail) == 0 && get16(tail + 4) == TAIL_BYTES && tail[6] == 0 &&
        tail[7] == EXT4_FT_DIR_CSUM;
}

void dir_init_tail(uint8_t *block) {
    uint8_t *tail = block + fs->block_size - TAIL_BYTES;
    memset(tail, 0, TAIL_BYTES);
    put16(tail + 4, TAIL_BYTES);
    tail[7] = EXT4_FT_DIR_CSUM;
}

void dir_block_seal(uint32_t seed, uint8_t *block) {
    if (!has_csum() || !dir_tail_present(block)) return;
    put32(block + fs->block_size - 4U, ext4_csum(seed, block, fs->block_size - TAIL_BYTES));
}

static uint32_t usable_size(const uint8_t *block) {
    return has_csum() && dir_tail_present(block) ? fs->block_size - TAIL_BYTES : fs->block_size;
}

static uint32_t new_block_usable(void) {
    return has_csum() ? fs->block_size - TAIL_BYTES : fs->block_size;
}

static uint32_t dx_root_limit(uint32_t info_length) {
    uint32_t space = fs->block_size - DX_INFO_AT - info_length;
    if (has_csum()) space -= DX_TAIL_BYTES;
    return space / DX_ENTRY_BYTES;
}

static uint32_t dx_node_limit(void) {
    uint32_t space = fs->block_size - DX_NODE_OFFSET;
    if (has_csum()) space -= DX_TAIL_BYTES;
    return space / DX_ENTRY_BYTES;
}

static void dx_seal(uint32_t seed, uint8_t *block, uint32_t count_offset) {
    if (!has_csum()) return;
    uint32_t limit = get16(block + count_offset);
    uint32_t count = get16(block + count_offset + 2U);
    uint32_t tail = count_offset + limit * DX_ENTRY_BYTES;
    if (tail + DX_TAIL_BYTES > fs->block_size || count > limit) return;
    uint32_t dummy = 0;
    uint32_t sum = ext4_csum(seed, block, count_offset + count * DX_ENTRY_BYTES);
    sum = ext4_csum(sum, block + tail, 4U);
    sum = ext4_csum(sum, &dummy, sizeof(dummy));
    put32(block + tail + 4U, sum);
}

static int is_dx_node(const uint8_t *block) {
    return get32(block) == 0 && get16(block + 4) == fs->block_size && block[6] == 0;
}

static int dir_indexed(const struct ext4_inode *dir) {
    return (dir->i_flags & EXT4_INDEX_FL) && has_compat(EXT4_COMPAT_DIR_INDEX);
}

void dir_seal_any(uint32_t dir_ino, const struct ext4_inode *dir, uint64_t lblk, uint8_t *block) {
    if (!has_csum()) return;
    uint32_t seed = inode_seed(dir_ino, dir);
    if (dir_tail_present(block)) {
        dir_block_seal(seed, block);
        return;
    }
    if (!dir_indexed(dir)) return;
    if (lblk == 0) dx_seal(seed, block, DX_INFO_AT + block[DX_INFO_AT + 5U]);
    else if (is_dx_node(block)) dx_seal(seed, block, DX_NODE_OFFSET);
}

static void dirent_fill(struct ext4_dir_entry *entry, uint32_t ino, const char *name,
                        size_t name_len, uint8_t file_type) {
    entry->inode = ino;
    entry->name_len = (uint8_t)name_len;
    entry->file_type = has_incompat(EXT4_INCOMPAT_FILETYPE) ? file_type : 0;
    memcpy(entry->name, name, name_len);
}

static uint8_t *dir_block(uint32_t dir_ino, struct ext4_inode *dir, uint64_t lblk) {
    int dirty = 0;
    int64_t block = inode_bmap(dir_ino, dir, lblk, 0, &dirty);
    if (block <= 0) return NULL;
    return buf_get((uint64_t)block);
}

static uint8_t *dir_append(uint32_t dir_ino, struct ext4_inode *dir, uint64_t *lblk_out) {
    uint64_t lblk = inode_size_of(dir) / fs->block_size;
    int dirty = 0;
    int64_t block = inode_bmap(dir_ino, dir, lblk, 1, &dirty);
    if (block <= 0) return NULL;
    uint8_t *data = buf_zero((uint64_t)block);
    if (!data) return NULL;
    inode_set_size(dir, (lblk + 1U) * fs->block_size);
    *lblk_out = lblk;
    return data;
}

static int block_insert(uint8_t *block, const char *name, size_t name_len, uint32_t child_ino,
                        uint8_t file_type) {
    uint32_t needed = dirent_rec_len((uint32_t)name_len);
    uint32_t size = usable_size(block);
    uint32_t at = 0;
    while (at + 8U <= size) {
        struct ext4_dir_entry *entry = (struct ext4_dir_entry *)(block + at);
        if (!dirent_sane(entry, at, size)) return -1;
        if (!entry->inode && entry->rec_len >= needed) {
            dirent_fill(entry, child_ino, name, name_len, file_type);
            return 1;
        }
        uint32_t used = dirent_rec_len(entry->name_len);
        if (entry->inode && entry->rec_len >= used + needed) {
            struct ext4_dir_entry *fresh = (struct ext4_dir_entry *)(block + at + used);
            fresh->rec_len = (uint16_t)(entry->rec_len - used);
            entry->rec_len = (uint16_t)used;
            dirent_fill(fresh, child_ino, name, name_len, file_type);
            return 1;
        }
        at += entry->rec_len;
    }
    return 0;
}

static void block_start_empty(uint8_t *block) {
    struct ext4_dir_entry *entry = (struct ext4_dir_entry *)block;
    entry->inode = 0;
    entry->rec_len = (uint16_t)new_block_usable();
    entry->name_len = 0;
    entry->file_type = 0;
    if (has_csum()) dir_init_tail(block);
}

struct dx_frame {
    uint64_t lblk;
    uint32_t count_offset;
    int at;
};

struct dx_probe {
    int levels;
    uint32_t version;
    uint32_t hash;
    struct dx_frame frame[DX_MAX_LEVELS];
    uint64_t leaf;
};

static int dx_max_levels(void) { return has_incompat(EXT4_INCOMPAT_LARGEDIR) ? 3 : 2; }

static uint32_t dx_hash_version(const uint8_t *root) {
    uint32_t version = root[DX_INFO_AT + 4U];
    if (version <= DX_HASH_TEA) version += fs->hash_unsigned;
    return version;
}

static uint32_t entry_hash(const uint8_t *block, uint32_t count_offset, int index) {
    return index ? get32(block + count_offset + (uint32_t)index * DX_ENTRY_BYTES) : 0;
}

static uint32_t entry_block(const uint8_t *block, uint32_t count_offset, int index) {
    return get32(block + count_offset + (uint32_t)index * DX_ENTRY_BYTES + 4U) & DX_BLOCK_MASK;
}

static int dx_probe(uint32_t dir_ino, struct ext4_inode *dir, const char *name, size_t name_len,
                    struct dx_probe *probe) {
    uint8_t *root = dir_block(dir_ino, dir, 0);
    if (!root) return -1;
    uint32_t info_length = root[DX_INFO_AT + 5U];
    uint32_t base_version = root[DX_INFO_AT + 4U];
    int levels = root[DX_INFO_AT + 6U];
    if (get32(root + DX_INFO_AT) != 0 || info_length < 8U || base_version > DX_HASH_TEA ||
        levels >= dx_max_levels())
        return -1;
    uint32_t count_offset = DX_INFO_AT + info_length;
    if (get16(root + count_offset) != dx_root_limit(info_length)) return -1;
    probe->levels = levels;
    probe->version = dx_hash_version(root);
    probe->hash = ext4_dirhash(name, name_len, probe->version, fs->hash_seed, NULL);
    uint64_t lblk = 0;
    uint8_t *block = root;
    for (int level = 0; level <= levels; level++) {
        uint32_t limit = get16(block + count_offset);
        uint32_t count = get16(block + count_offset + 2U);
        if (!count || count > limit) return -1;
        int low = 1;
        int high = (int)count - 1;
        while (low <= high) {
            int middle = low + (high - low) / 2;
            if (entry_hash(block, count_offset, middle) > probe->hash) high = middle - 1;
            else low = middle + 1;
        }
        int at = low - 1;
        probe->frame[level].lblk = lblk;
        probe->frame[level].count_offset = count_offset;
        probe->frame[level].at = at;
        lblk = entry_block(block, count_offset, at);
        if (level == levels) break;
        block = dir_block(dir_ino, dir, lblk);
        if (!block || !is_dx_node(block)) return -1;
        count_offset = DX_NODE_OFFSET;
        if (get16(block + count_offset) != dx_node_limit()) return -1;
    }
    probe->leaf = lblk;
    return 0;
}

static int frame_full(uint32_t dir_ino, struct ext4_inode *dir, const struct dx_frame *frame) {
    uint8_t *block = dir_block(dir_ino, dir, frame->lblk);
    if (!block) return -1;
    return get16(block + frame->count_offset + 2U) >= get16(block + frame->count_offset);
}

static int dx_insert_entry(uint32_t dir_ino, struct ext4_inode *dir, const struct dx_frame *frame,
                           uint32_t hash, uint64_t lblk) {
    uint8_t *block = dir_block(dir_ino, dir, frame->lblk);
    if (!block) return -1;
    uint32_t co = frame->count_offset;
    uint32_t count = get16(block + co + 2U);
    if (count >= get16(block + co)) return -1;
    uint32_t at = (uint32_t)frame->at + 1U;
    uint8_t *slot = block + co + at * DX_ENTRY_BYTES;
    memmove(slot + DX_ENTRY_BYTES, slot, (size_t)(count - at) * DX_ENTRY_BYTES);
    put32(slot, hash);
    put32(slot + 4U, (uint32_t)lblk);
    put16(block + co + 2U, (uint16_t)(count + 1U));
    dx_seal(inode_seed(dir_ino, dir), block, co);
    buf_mark(block);
    return 0;
}

struct map_entry {
    uint32_t hash;
    uint32_t offset;
    uint32_t size;
};

static void sort_map(struct map_entry *map, uint32_t count) {
    for (uint32_t index = 1; index < count; index++) {
        struct map_entry item = map[index];
        uint32_t at = index;
        while (at > 0 && map[at - 1U].hash > item.hash) {
            map[at] = map[at - 1U];
            at--;
        }
        map[at] = item;
    }
}

static uint32_t pack_entries(uint8_t *out, const uint8_t *source, const struct map_entry *map,
                             uint32_t count) {
    uint32_t at = 0;
    struct ext4_dir_entry *last = NULL;
    for (uint32_t index = 0; index < count; index++) {
        const struct ext4_dir_entry *entry =
            (const struct ext4_dir_entry *)(source + map[index].offset);
        uint32_t size = dirent_rec_len(entry->name_len);
        memcpy(out + at, entry, 8U + entry->name_len);
        last = (struct ext4_dir_entry *)(out + at);
        last->rec_len = (uint16_t)size;
        at += size;
    }
    uint32_t usable = new_block_usable();
    if (last) {
        last->rec_len = (uint16_t)(last->rec_len + usable - at);
    } else {
        struct ext4_dir_entry *empty = (struct ext4_dir_entry *)out;
        empty->inode = 0;
        empty->rec_len = (uint16_t)usable;
        empty->name_len = 0;
        empty->file_type = 0;
    }
    if (has_csum()) dir_init_tail(out);
    return at;
}

static int split_leaf(uint32_t dir_ino, struct ext4_inode *dir, struct dx_probe *probe) {
    uint8_t *leaf = dir_block(dir_ino, dir, probe->leaf);
    if (!leaf) return -1;
    uint8_t *copy = (uint8_t *)kmalloc(fs->block_size);
    struct map_entry *map =
        (struct map_entry *)kmalloc((fs->block_size / 12U + 1U) * sizeof(struct map_entry));
    if (!copy || !map) {
        kfree(copy);
        kfree(map);
        return -1;
    }
    memcpy(copy, leaf, fs->block_size);
    uint32_t size = usable_size(copy);
    uint32_t count = 0;
    for (uint32_t at = 0; at + 8U <= size;) {
        struct ext4_dir_entry *entry = (struct ext4_dir_entry *)(copy + at);
        if (!dirent_sane(entry, at, size)) {
            kfree(copy);
            kfree(map);
            return -1;
        }
        if (entry->inode && entry->name_len) {
            map[count].hash =
                ext4_dirhash(entry->name, entry->name_len, probe->version, fs->hash_seed, NULL);
            map[count].offset = at;
            map[count].size = dirent_rec_len(entry->name_len);
            count++;
        }
        at += entry->rec_len;
    }
    if (count < 2U) {
        kfree(copy);
        kfree(map);
        return -1;
    }
    sort_map(map, count);
    uint32_t moved = 0;
    uint32_t move = 0;
    int index;
    for (index = (int)count - 1; index >= 0; index--) {
        if (moved + map[index].size / 2U > fs->block_size / 2U) break;
        moved += map[index].size;
        move++;
    }
    uint32_t split = index > 0 ? count - move : count / 2U;
    if (split == 0) split = 1;
    uint32_t hash2 = map[split].hash;
    uint32_t continued = hash2 == map[split - 1U].hash ? 1U : 0U;

    uint64_t new_lblk;
    uint8_t *fresh = dir_append(dir_ino, dir, &new_lblk);
    if (!fresh) {
        kfree(copy);
        kfree(map);
        return -1;
    }
    uint32_t seed = inode_seed(dir_ino, dir);
    pack_entries(fresh, copy, map + split, count - split);
    dir_block_seal(seed, fresh);
    leaf = dir_block(dir_ino, dir, probe->leaf);
    if (!leaf) {
        kfree(copy);
        kfree(map);
        return -1;
    }
    memset(leaf, 0, fs->block_size);
    pack_entries(leaf, copy, map, split);
    dir_block_seal(seed, leaf);
    buf_mark(leaf);
    kfree(copy);
    kfree(map);
    return dx_insert_entry(dir_ino, dir, &probe->frame[probe->levels], hash2 + continued, new_lblk);
}

static void node_init(uint8_t *block, uint32_t count) {
    put32(block, 0);
    put16(block + 4, (uint16_t)fs->block_size);
    block[6] = 0;
    block[7] = 0;
    put16(block + DX_NODE_OFFSET, (uint16_t)dx_node_limit());
    put16(block + DX_NODE_OFFSET + 2U, (uint16_t)count);
}

static int split_node(uint32_t dir_ino, struct ext4_inode *dir, struct dx_probe *probe, int level) {
    struct dx_frame *frame = &probe->frame[level];
    uint8_t *node = dir_block(dir_ino, dir, frame->lblk);
    if (!node) return -1;
    uint32_t co = frame->count_offset;
    uint32_t count = get16(node + co + 2U);
    uint32_t keep = count / 2U;
    uint32_t moving = count - keep;
    uint32_t hash2 = get32(node + co + keep * DX_ENTRY_BYTES);
    uint8_t *saved = (uint8_t *)kmalloc((size_t)moving * DX_ENTRY_BYTES);
    if (!saved) return -1;
    memcpy(saved, node + co + keep * DX_ENTRY_BYTES, (size_t)moving * DX_ENTRY_BYTES);
    uint64_t new_lblk;
    uint8_t *fresh = dir_append(dir_ino, dir, &new_lblk);
    if (!fresh) {
        kfree(saved);
        return -1;
    }
    memcpy(fresh + DX_NODE_OFFSET, saved, (size_t)moving * DX_ENTRY_BYTES);
    kfree(saved);
    node_init(fresh, moving);
    uint32_t seed = inode_seed(dir_ino, dir);
    dx_seal(seed, fresh, DX_NODE_OFFSET);
    node = dir_block(dir_ino, dir, frame->lblk);
    if (!node) return -1;
    memset(node + co + keep * DX_ENTRY_BYTES, 0, (size_t)moving * DX_ENTRY_BYTES);
    put16(node + co + 2U, (uint16_t)keep);
    dx_seal(seed, node, co);
    buf_mark(node);
    return dx_insert_entry(dir_ino, dir, &probe->frame[level - 1], hash2, new_lblk);
}

static int add_level(uint32_t dir_ino, struct ext4_inode *dir, struct dx_probe *probe) {
    uint8_t *root = dir_block(dir_ino, dir, 0);
    if (!root) return -1;
    uint32_t co = probe->frame[0].count_offset;
    uint32_t count = get16(root + co + 2U);
    uint8_t *saved = (uint8_t *)kmalloc((size_t)count * DX_ENTRY_BYTES);
    if (!saved) return -1;
    memcpy(saved, root + co, (size_t)count * DX_ENTRY_BYTES);
    uint64_t new_lblk;
    uint8_t *fresh = dir_append(dir_ino, dir, &new_lblk);
    if (!fresh) {
        kfree(saved);
        return -1;
    }
    memcpy(fresh + DX_NODE_OFFSET, saved, (size_t)count * DX_ENTRY_BYTES);
    kfree(saved);
    node_init(fresh, count);
    uint32_t seed = inode_seed(dir_ino, dir);
    dx_seal(seed, fresh, DX_NODE_OFFSET);
    root = dir_block(dir_ino, dir, 0);
    if (!root) return -1;
    memset(root + co + DX_ENTRY_BYTES, 0, (size_t)(count - 1U) * DX_ENTRY_BYTES);
    put16(root + co + 2U, 1);
    put32(root + co + 4U, (uint32_t)new_lblk);
    root[DX_INFO_AT + 6U]++;
    dx_seal(seed, root, co);
    buf_mark(root);
    return 0;
}

static int dx_add(uint32_t dir_ino, struct ext4_inode *dir, const char *name, size_t name_len,
                  uint32_t child_ino, uint8_t file_type) {
    for (int attempt = 0; attempt < 32; attempt++) {
        struct dx_probe probe;
        if (dx_probe(dir_ino, dir, name, name_len, &probe) != 0) return -2;
        uint8_t *leaf = dir_block(dir_ino, dir, probe.leaf);
        if (!leaf) return -1;
        int added = block_insert(leaf, name, name_len, child_ino, file_type);
        if (added < 0) return -1;
        if (added) {
            dir_block_seal(inode_seed(dir_ino, dir), leaf);
            buf_mark(leaf);
            return 0;
        }
        int last = probe.levels;
        int full = frame_full(dir_ino, dir, &probe.frame[last]);
        if (full < 0) return -1;
        if (!full) {
            if (split_leaf(dir_ino, dir, &probe) != 0) return -1;
            continue;
        }
        int level = last;
        while (level > 0) {
            int above = frame_full(dir_ino, dir, &probe.frame[level - 1]);
            if (above < 0) return -1;
            if (!above) break;
            level--;
        }
        if (level == 0) {
            if (probe.levels + 1 >= dx_max_levels()) {
                kprintf("EXT4: %s directory inode %u index is full\n", fs->device->dev_name,
                        (unsigned)dir_ino);
                return -1;
            }
            if (add_level(dir_ino, dir, &probe) != 0) return -1;
        } else if (split_node(dir_ino, dir, &probe, level) != 0) {
            return -1;
        }
    }
    return -1;
}

static int make_indexed(uint32_t dir_ino, struct ext4_inode *dir) {
    uint8_t *root = dir_block(dir_ino, dir, 0);
    if (!root) return -1;
    struct ext4_dir_entry *dot = (struct ext4_dir_entry *)root;
    if (dot->rec_len != 12U || dot->name_len != 1U || dot->name[0] != '.') return -1;
    struct ext4_dir_entry *dotdot = (struct ext4_dir_entry *)(root + 12);
    if (dotdot->name_len != 2U || dotdot->name[0] != '.' || dotdot->name[1] != '.') return -1;
    uint32_t parent = dotdot->inode;

    uint8_t *copy = (uint8_t *)kmalloc(fs->block_size);
    struct map_entry *map =
        (struct map_entry *)kmalloc((fs->block_size / 12U + 1U) * sizeof(struct map_entry));
    if (!copy || !map) {
        kfree(copy);
        kfree(map);
        return -1;
    }
    memcpy(copy, root, fs->block_size);
    uint32_t size = usable_size(copy);
    uint32_t count = 0;
    uint32_t at = 12U + ((struct ext4_dir_entry *)(copy + 12))->rec_len;
    if (((struct ext4_dir_entry *)(copy + 12))->rec_len < 12U) at = size;
    while (at + 8U <= size) {
        struct ext4_dir_entry *entry = (struct ext4_dir_entry *)(copy + at);
        if (!dirent_sane(entry, at, size)) break;
        if (entry->inode && entry->name_len) {
            map[count].hash = 0;
            map[count].offset = at;
            map[count].size = dirent_rec_len(entry->name_len);
            count++;
        }
        at += entry->rec_len;
    }

    uint64_t leaf_lblk;
    uint8_t *leaf = dir_append(dir_ino, dir, &leaf_lblk);
    if (!leaf) {
        kfree(copy);
        kfree(map);
        return -1;
    }
    uint32_t seed = inode_seed(dir_ino, dir);
    pack_entries(leaf, copy, map, count);
    dir_block_seal(seed, leaf);
    kfree(copy);
    kfree(map);

    root = dir_block(dir_ino, dir, 0);
    if (!root) return -1;
    memset(root, 0, fs->block_size);
    dot = (struct ext4_dir_entry *)root;
    dot->inode = dir_ino;
    dot->rec_len = 12;
    dot->name_len = 1;
    dot->file_type = has_incompat(EXT4_INCOMPAT_FILETYPE) ? EXT4_FT_DIR : 0;
    dot->name[0] = '.';
    dotdot = (struct ext4_dir_entry *)(root + 12);
    dotdot->inode = parent;
    dotdot->rec_len = (uint16_t)(fs->block_size - 12U);
    dotdot->name_len = 2;
    dotdot->file_type = has_incompat(EXT4_INCOMPAT_FILETYPE) ? EXT4_FT_DIR : 0;
    dotdot->name[0] = '.';
    dotdot->name[1] = '.';
    uint8_t version = fs->sb.s_def_hash_version;
    if (version > DX_HASH_TEA) version = DX_HASH_HALF_MD4;
    root[DX_INFO_AT + 4U] = version;
    root[DX_INFO_AT + 5U] = 8U;
    uint32_t co = DX_INFO_AT + 8U;
    put16(root + co, (uint16_t)dx_root_limit(8U));
    put16(root + co + 2U, 1);
    put32(root + co + 4U, (uint32_t)leaf_lblk);
    dir->i_flags |= EXT4_INDEX_FL;
    dx_seal(seed, root, co);
    buf_mark(root);
    return 0;
}

static int linear_add(uint32_t dir_ino, struct ext4_inode *dir, const char *name, size_t name_len,
                      uint32_t child_ino, uint8_t file_type) {
    uint64_t blocks = inode_size_of(dir) / fs->block_size;
    uint32_t seed = inode_seed(dir_ino, dir);
    for (uint64_t lblk = 0; lblk < blocks; lblk++) {
        uint8_t *block = dir_block(dir_ino, dir, lblk);
        if (!block) return -1;
        int added = block_insert(block, name, name_len, child_ino, file_type);
        if (added < 0) return -1;
        if (added) {
            dir_block_seal(seed, block);
            buf_mark(block);
            return 0;
        }
    }
    if (blocks == 1U && has_compat(EXT4_COMPAT_DIR_INDEX) &&
        !(dir->i_flags & EXT4_INLINE_DATA_FL) && make_indexed(dir_ino, dir) == 0)
        return 1;
    uint64_t lblk;
    uint8_t *block = dir_append(dir_ino, dir, &lblk);
    if (!block) return -1;
    block_start_empty(block);
    if (block_insert(block, name, name_len, child_ino, file_type) != 1) return -1;
    dir_block_seal(seed, block);
    return 0;
}

int dir_add_entry(uint32_t dir_ino, const char *name, uint32_t child_ino, uint8_t file_type) {
    size_t name_len = strlen(name);
    if (!name_len || name_len > EXT4_NAME_LEN) return -1;
    struct ext4_inode dir;
    if (inode_read(dir_ino, &dir) != 0) return -1;
    if ((dir.i_flags & EXT4_INLINE_DATA_FL) && inline_expand(dir_ino, &dir) != 0) return -1;
    int status;
    if (dir_indexed(&dir)) {
        status = dx_add(dir_ino, &dir, name, name_len, child_ino, file_type);
        if (status == -2) {
            dir.i_flags &= ~EXT4_INDEX_FL;
            status = linear_add(dir_ino, &dir, name, name_len, child_ino, file_type);
        }
    } else {
        status = linear_add(dir_ino, &dir, name, name_len, child_ino, file_type);
        if (status == 1) status = dx_add(dir_ino, &dir, name, name_len, child_ino, file_type);
    }
    if (status != 0) {
        inode_write(dir_ino, &dir);
        return -1;
    }
    uint32_t seconds;
    uint32_t nanoseconds;
    ext4_now_ns(&seconds, &nanoseconds);
    inode_set_times(&dir, dir.i_atime, dir.i_atime_extra >> 2, seconds, nanoseconds, seconds,
                    nanoseconds);
    return inode_write(dir_ino, &dir);
}

static int block_remove(uint8_t *block, const char *name, size_t name_len) {
    uint32_t size = usable_size(block);
    uint32_t at = 0;
    struct ext4_dir_entry *previous = NULL;
    while (at + 8U <= size) {
        struct ext4_dir_entry *entry = (struct ext4_dir_entry *)(block + at);
        if (!dirent_sane(entry, at, size)) return -1;
        if (entry->inode && entry->name_len == name_len &&
            memcmp(entry->name, name, name_len) == 0) {
            if (previous) previous->rec_len = (uint16_t)(previous->rec_len + entry->rec_len);
            else entry->inode = 0;
            return 1;
        }
        previous = entry;
        at += entry->rec_len;
    }
    return 0;
}

int dir_remove_entry(uint32_t dir_ino, const char *name) {
    size_t name_len = strlen(name);
    struct ext4_inode dir;
    if (inode_read(dir_ino, &dir) != 0) return -1;
    if ((dir.i_flags & EXT4_INLINE_DATA_FL) && inline_expand(dir_ino, &dir) != 0) return -1;
    uint64_t blocks = inode_size_of(&dir) / fs->block_size;
    uint64_t hinted = UINT64_MAX;
    if (dir_indexed(&dir)) {
        struct dx_probe probe;
        if (dx_probe(dir_ino, &dir, name, name_len, &probe) == 0) {
            hinted = probe.leaf;
            uint8_t *block = dir_block(dir_ino, &dir, hinted);
            if (block && block_remove(block, name, name_len) == 1) {
                dir_seal_any(dir_ino, &dir, hinted, block);
                buf_mark(block);
                return inode_write(dir_ino, &dir);
            }
        }
    }
    for (uint64_t lblk = 0; lblk < blocks; lblk++) {
        if (lblk == hinted) continue;
        uint8_t *block = dir_block(dir_ino, &dir, lblk);
        if (!block) continue;
        int removed = block_remove(block, name, name_len);
        if (removed < 0) continue;
        if (removed) {
            dir_seal_any(dir_ino, &dir, lblk, block);
            buf_mark(block);
            return inode_write(dir_ino, &dir);
        }
    }
    inode_write(dir_ino, &dir);
    return -1;
}

int dir_set_dotdot(uint32_t dir_ino, uint32_t parent_ino) {
    struct ext4_inode dir;
    if (inode_read(dir_ino, &dir) != 0) return -1;
    if (dir.i_flags & EXT4_INLINE_DATA_FL) {
        dir.i_block[0] = parent_ino;
        return inode_write(dir_ino, &dir);
    }
    uint8_t *block = dir_block(dir_ino, &dir, 0);
    if (!block) return -1;
    struct ext4_dir_entry *dot = (struct ext4_dir_entry *)block;
    if (dot->rec_len < 12U || dot->rec_len >= fs->block_size) return -1;
    struct ext4_dir_entry *dotdot = (struct ext4_dir_entry *)(block + dot->rec_len);
    if (dotdot->name_len != 2 || dotdot->name[0] != '.' || dotdot->name[1] != '.') return -1;
    dotdot->inode = parent_ino;
    dir_seal_any(dir_ino, &dir, 0, block);
    buf_mark(block);
    return 0;
}

int dir_create_block(uint32_t dir_ino, struct ext4_inode *dir, uint32_t parent_ino) {
    int dirty = 0;
    int64_t physical = inode_bmap(dir_ino, dir, 0, 1, &dirty);
    if (physical <= 0) return -1;
    uint8_t *block = buf_zero((uint64_t)physical);
    if (!block) return -1;
    uint8_t type = has_incompat(EXT4_INCOMPAT_FILETYPE) ? EXT4_FT_DIR : 0;
    struct ext4_dir_entry *dot = (struct ext4_dir_entry *)block;
    dot->inode = dir_ino;
    dot->rec_len = 12;
    dot->name_len = 1;
    dot->file_type = type;
    dot->name[0] = '.';
    struct ext4_dir_entry *dotdot = (struct ext4_dir_entry *)(block + 12);
    dotdot->inode = parent_ino;
    dotdot->rec_len = (uint16_t)(new_block_usable() - 12U);
    dotdot->name_len = 2;
    dotdot->file_type = type;
    dotdot->name[0] = '.';
    dotdot->name[1] = '.';
    if (has_csum()) dir_init_tail(block);
    inode_set_size(dir, fs->block_size);
    dir_block_seal(inode_seed(dir_ino, dir), block);
    return 0;
}
