#include "ext4.h"

#include <tunix/acl.h>
#include <tunix/heap.h>
#include <tunix/kstring.h>
#include <tunix/xattr.h>

#define HEADER_BYTES     32U
#define ENTRY_BYTES      16U
#define ACL_DISK_VERSION 1U
#define ACL_ABI_VERSION  2U
#define ACL_USER_OBJ     0x01U
#define ACL_USER         0x02U
#define ACL_GROUP_OBJ    0x04U
#define ACL_GROUP        0x08U
#define ACL_MASK         0x10U
#define ACL_OTHER        0x20U
#define ACL_NO_ID        0xFFFFFFFFU

static uint32_t entry_len(uint32_t name_len) { return (ENTRY_BYTES + name_len + 3U) & ~3U; }
static uint32_t value_len(uint32_t size) { return (size + 3U) & ~3U; }

void xattr_set_free(struct ext4_xattr_set *set) {
    for (uint32_t index = 0; index < set->count; index++) kfree(set->items[index].value);
    kfree(set->items);
    set->items = NULL;
    set->count = 0;
    set->capacity = 0;
}

static struct ext4_xattr *set_append(struct ext4_xattr_set *set) {
    if (set->count == set->capacity) {
        uint32_t capacity = set->capacity ? set->capacity * 2U : 8U;
        struct ext4_xattr *items = (struct ext4_xattr *)kmalloc(capacity * sizeof(*items));
        if (!items) return NULL;
        if (set->count) memcpy(items, set->items, set->count * sizeof(*items));
        kfree(set->items);
        set->items = items;
        set->capacity = capacity;
    }
    struct ext4_xattr *item = &set->items[set->count++];
    memset(item, 0, sizeof(*item));
    return item;
}

struct ext4_xattr *xattr_set_find(struct ext4_xattr_set *set, uint8_t index, const char *name) {
    size_t length = strlen(name);
    for (uint32_t at = 0; at < set->count; at++) {
        struct ext4_xattr *item = &set->items[at];
        if (item->index == index && item->name_len == length &&
            memcmp(item->name, name, length) == 0)
            return item;
    }
    return NULL;
}

int xattr_set_remove(struct ext4_xattr_set *set, uint8_t index, const char *name) {
    struct ext4_xattr *item = xattr_set_find(set, index, name);
    if (!item) return -1;
    kfree(item->value);
    uint32_t at = (uint32_t)(item - set->items);
    memmove(item, item + 1, (set->count - at - 1U) * sizeof(*item));
    set->count--;
    return 0;
}

int xattr_set_put(struct ext4_xattr_set *set, uint8_t index, const char *name, const void *value,
                  uint32_t size) {
    size_t length = strlen(name);
    if (length > EXT4_NAME_LEN) return -1;
    uint8_t *copy = NULL;
    if (size) {
        copy = (uint8_t *)kmalloc(size);
        if (!copy) return -1;
        memcpy(copy, value, size);
    }
    struct ext4_xattr *item = xattr_set_find(set, index, name);
    if (!item) item = set_append(set);
    if (!item) {
        kfree(copy);
        return -1;
    }
    kfree(item->value);
    item->index = index;
    item->name_len = (uint8_t)length;
    memcpy(item->name, name, length);
    item->name[length] = '\0';
    item->value = copy;
    item->value_size = size;
    item->value_inum = 0;
    item->ea_hash = 0;
    return 0;
}

static int read_ea_inode(uint32_t ea_ino, uint32_t size, uint8_t *out) {
    struct ext4_inode inode;
    if (inode_read(ea_ino, &inode) != 0 || !(inode.i_flags & EXT4_EA_INODE_FL) ||
        inode_size_of(&inode) < size)
        return -1;
    for (uint32_t done = 0; done < size;) {
        uint32_t run = 0;
        int unwritten = 0;
        int64_t block = inode_map(ea_ino, &inode, done / fs->block_size, &run, &unwritten);
        uint32_t chunk = fs->block_size;
        if (chunk > size - done) chunk = size - done;
        if (block <= 0 || unwritten) memset(out + done, 0, chunk);
        else if (read_block((uint64_t)block, ext4_scratch) != 0) return -1;
        else memcpy(out + done, ext4_scratch, chunk);
        done += chunk;
    }
    return 0;
}

static int parse_entries(struct ext4_xattr_set *set, const uint8_t *first, const uint8_t *end,
                         const uint8_t *value_base) {
    const uint8_t *entry = first;
    while (entry + 4U <= end && get32(entry) != 0) {
        const struct ext4_xattr_entry *raw = (const struct ext4_xattr_entry *)entry;
        uint32_t length = entry_len(raw->e_name_len);
        if (entry + length > end) return -1;
        struct ext4_xattr *item = set_append(set);
        if (!item) return -1;
        item->index = raw->e_name_index;
        item->name_len = raw->e_name_len;
        memcpy(item->name, raw->e_name, raw->e_name_len);
        item->name[raw->e_name_len] = '\0';
        item->value_size = raw->e_value_size;
        item->value_inum = raw->e_value_inum;
        item->ea_hash = raw->e_hash;
        if (raw->e_value_size) {
            item->value = (uint8_t *)kmalloc(raw->e_value_size);
            if (!item->value) return -1;
            if (raw->e_value_inum) {
                if (read_ea_inode(raw->e_value_inum, raw->e_value_size, item->value) != 0)
                    return -1;
            } else {
                const uint8_t *value = value_base + raw->e_value_offs;
                if (value < value_base || value + raw->e_value_size > end) return -1;
                memcpy(item->value, value, raw->e_value_size);
            }
        }
        entry += length;
    }
    return 0;
}

static uint8_t *ibody_base(uint32_t ino, const struct ext4_inode *inode, uint8_t **table) {
    if (fs->inode_size <= EXT4_GOOD_OLD_INODE_SIZE) return NULL;
    uint8_t *slot = inode_slot(ino, table);
    if (!slot) return NULL;
    uint32_t start = EXT4_GOOD_OLD_INODE_SIZE + inode->i_extra_isize;
    if (start + 8U > fs->inode_size) return NULL;
    return slot + start;
}

uint32_t xattr_ibody_space(const struct ext4_inode *inode) {
    if (fs->inode_size <= EXT4_GOOD_OLD_INODE_SIZE) return 0;
    uint32_t start = EXT4_GOOD_OLD_INODE_SIZE + inode->i_extra_isize;
    if (start + 8U > fs->inode_size) return 0;
    return fs->inode_size - start - 8U;
}

int xattr_load(uint32_t ino, const struct ext4_inode *inode, struct ext4_xattr_set *out) {
    memset(out, 0, sizeof(*out));
    uint8_t *base = ibody_base(ino, inode, NULL);
    if (base && get32(base) == EXT4_XATTR_MAGIC) {
        uint8_t *slot = inode_slot(ino, NULL);
        if (parse_entries(out, base + 4U, slot + fs->inode_size, base + 4U) != 0) {
            xattr_set_free(out);
            return -1;
        }
    }
    uint64_t block = inode_file_acl(inode);
    if (block) {
        uint8_t *data = buf_get(block);
        if (!data || get32(data) != EXT4_XATTR_MAGIC || get32(data + 8) != 1U) {
            xattr_set_free(out);
            return -1;
        }
        uint8_t *copy = (uint8_t *)kmalloc(fs->block_size);
        if (!copy) {
            xattr_set_free(out);
            return -1;
        }
        memcpy(copy, data, fs->block_size);
        int status = parse_entries(out, copy + HEADER_BYTES, copy + fs->block_size, copy);
        kfree(copy);
        if (status != 0) {
            xattr_set_free(out);
            return -1;
        }
    }
    return 0;
}

static uint32_t hash_entry(const char *name, uint32_t name_len, const uint8_t *value,
                           uint32_t value_size) {
    uint32_t hash = 0;
    for (uint32_t index = 0; index < name_len; index++)
        hash = (hash << 5) ^ (hash >> 27) ^ (uint8_t)name[index];
    for (uint32_t at = 0; at < value_len(value_size); at += 4U) {
        uint32_t word = 0;
        for (uint32_t byte = 0; byte < 4U; byte++)
            if (at + byte < value_size) word |= (uint32_t)value[at + byte] << (8U * byte);
        hash = (hash << 16) ^ (hash >> 16) ^ word;
    }
    return hash;
}

static uint32_t item_hash(const struct ext4_xattr *item) {
    if (item->value_inum) return item->ea_hash;
    return hash_entry(item->name, item->name_len, item->value, item->value_size);
}

static uint32_t item_room(const struct ext4_xattr *item) {
    return entry_len(item->name_len) + (item->value_inum ? 0U : value_len(item->value_size));
}

static void put_entry(uint8_t *at, const struct ext4_xattr *item, uint32_t value_offset,
                      uint32_t hash) {
    struct ext4_xattr_entry *raw = (struct ext4_xattr_entry *)at;
    memset(at, 0, entry_len(item->name_len));
    raw->e_name_len = item->name_len;
    raw->e_name_index = item->index;
    raw->e_value_offs = item->value_inum ? 0 : (uint16_t)value_offset;
    raw->e_value_inum = item->value_inum;
    raw->e_value_size = item->value_size;
    raw->e_hash = hash;
    memcpy(raw->e_name, item->name, item->name_len);
}

static int item_before(const struct ext4_xattr *a, const struct ext4_xattr *b) {
    if (a->index != b->index) return a->index < b->index;
    if (a->name_len != b->name_len) return a->name_len < b->name_len;
    return memcmp(a->name, b->name, a->name_len) < 0;
}

static void block_csum_set(uint8_t *data, uint64_t block) {
    if (!has_csum()) return;
    uint32_t dummy = 0;
    uint32_t sum = ext4_csum(fs->csum_seed, &block, sizeof(block));
    sum = ext4_csum(sum, data, 16U);
    sum = ext4_csum(sum, &dummy, sizeof(dummy));
    sum = ext4_csum(sum, data + 20U, fs->block_size - 20U);
    put32(data + 16U, sum);
}

static void block_rehash(uint8_t *data) {
    uint32_t hash = 0;
    for (uint8_t *entry = data + HEADER_BYTES; get32(entry) != 0;) {
        const struct ext4_xattr_entry *raw = (const struct ext4_xattr_entry *)entry;
        if (!raw->e_hash) {
            hash = 0;
            break;
        }
        hash = (hash << 16) ^ (hash >> 16) ^ raw->e_hash;
        entry += entry_len(raw->e_name_len);
    }
    put32(data + 12U, hash);
}

static void ea_inode_deref(uint32_t ea_ino) {
    struct ext4_inode inode;
    if (inode_read(ea_ino, &inode) != 0 || !(inode.i_flags & EXT4_EA_INODE_FL)) return;
    uint64_t refs = (uint64_t)inode.i_ctime << 32 | inode.i_version;
    if (refs) refs--;
    if (refs) {
        inode.i_ctime = (uint32_t)(refs >> 32);
        inode.i_version = (uint32_t)refs;
        inode_write(ea_ino, &inode);
        return;
    }
    inode_release_blocks(ea_ino, &inode);
    inode.i_links_count = 0;
    inode.i_dtime = ext4_now();
    inode_set_size(&inode, 0);
    inode_write(ea_ino, &inode);
    free_inode(ea_ino, 0);
}

static void block_deref(struct ext4_inode *inode, uint64_t block) {
    uint8_t *data = buf_get(block);
    if (data && get32(data) == EXT4_XATTR_MAGIC && get32(data + 4) > 1U) {
        put32(data + 4, get32(data + 4) - 1U);
        block_csum_set(data, block);
        buf_mark(data);
    } else {
        free_block(block);
    }
    inode_uncount_block(inode);
}

static int write_ibody(uint32_t ino, const struct ext4_inode *inode, struct ext4_xattr **items,
                       uint32_t count) {
    uint8_t *table = NULL;
    uint8_t *base = ibody_base(ino, inode, &table);
    if (!base) return count ? -1 : 0;
    uint8_t *slot = inode_slot(ino, NULL);
    uint8_t *end = slot + fs->inode_size;
    memset(base, 0, (size_t)(end - base));
    if (!count) {
        buf_mark(table);
        return 0;
    }
    put32(base, EXT4_XATTR_MAGIC);
    uint8_t *first = base + 4U;
    uint8_t *entry = first;
    uint8_t *values = end;
    for (uint32_t index = 0; index < count; index++) {
        struct ext4_xattr *item = items[index];
        uint32_t offset = 0;
        if (!item->value_inum && item->value_size) {
            values -= value_len(item->value_size);
            memcpy(values, item->value, item->value_size);
            offset = (uint32_t)(values - first);
        }
        put_entry(entry, item, offset, item->value_inum ? item_hash(item) : 0);
        entry += entry_len(item->name_len);
    }
    buf_mark(table);
    return 0;
}

static int build_block(uint8_t *data, struct ext4_xattr **items, uint32_t count) {
    memset(data, 0, fs->block_size);
    put32(data, EXT4_XATTR_MAGIC);
    put32(data + 4, 1U);
    put32(data + 8, 1U);
    uint8_t *entry = data + HEADER_BYTES;
    uint8_t *values = data + fs->block_size;
    for (uint32_t index = 0; index < count; index++) {
        struct ext4_xattr *item = items[index];
        uint32_t offset = 0;
        if (!item->value_inum && item->value_size) {
            values -= value_len(item->value_size);
            if (values < entry + entry_len(item->name_len) + 4U) return -1;
            memcpy(values, item->value, item->value_size);
            offset = (uint32_t)(values - data);
        }
        if (entry + entry_len(item->name_len) + 4U > values) return -1;
        put_entry(entry, item, offset, item_hash(item));
        entry += entry_len(item->name_len);
    }
    block_rehash(data);
    return 0;
}

int xattr_store(uint32_t ino, struct ext4_inode *inode, const struct ext4_xattr_set *set) {
    struct ext4_xattr_set old;
    int have_old = xattr_load(ino, inode, &old) == 0;
    uint32_t count = set->count;
    struct ext4_xattr **order =
        (struct ext4_xattr **)kmalloc((count ? count : 1U) * sizeof(*order));
    struct ext4_xattr **inside =
        (struct ext4_xattr **)kmalloc((count ? count : 1U) * sizeof(*inside));
    struct ext4_xattr **outside =
        (struct ext4_xattr **)kmalloc((count ? count : 1U) * sizeof(*outside));
    uint8_t *image = (uint8_t *)kmalloc(fs->block_size);
    int status = -1;
    if (!order || !inside || !outside || !image) goto done;
    for (uint32_t index = 0; index < count; index++) order[index] = &set->items[index];
    for (uint32_t index = 1; index < count; index++) {
        struct ext4_xattr *key = order[index];
        uint32_t at = index;
        int key_first = key->index == EXT4_XATTR_INDEX_SYSTEM && key->name_len == 4U &&
            memcmp(key->name, "data", 4U) == 0;
        while (at && (key_first || item_before(key, order[at - 1U]))) {
            order[at] = order[at - 1U];
            at--;
        }
        order[at] = key;
    }

    uint32_t room = xattr_ibody_space(inode);
    uint32_t used = 0;
    uint32_t inside_count = 0;
    uint32_t outside_count = 0;
    for (uint32_t index = 0; index < count; index++) {
        uint32_t need = item_room(order[index]);
        if (used + need <= room) {
            inside[inside_count++] = order[index];
            used += need;
        } else {
            outside[outside_count++] = order[index];
        }
    }
    if (outside_count && !has_compat(EXT4_COMPAT_EXT_ATTR))
        fs->sb.s_feature_compat |= EXT4_COMPAT_EXT_ATTR;
    if (outside_count && build_block(image, outside, outside_count) != 0) {
        status = -ENOSPC;
        goto done;
    }
    if (write_ibody(ino, inode, inside, inside_count) != 0) goto done;

    uint64_t current = inode_file_acl(inode);
    if (!outside_count) {
        if (current) block_deref(inode, current);
        inode_set_file_acl(inode, 0);
    } else {
        uint64_t target = 0;
        if (current) {
            uint8_t *data = buf_get(current);
            if (data && get32(data) == EXT4_XATTR_MAGIC && get32(data + 4) == 1U) target = current;
            else block_deref(inode, current);
        }
        if (!target) {
            target = alloc_block(inode_goal(ino, inode, 0), 0);
            if (!target) goto done;
            inode_count_block(inode);
        }
        block_csum_set(image, target);
        if (buf_write(target, image) != 0) goto done;
        inode_set_file_acl(inode, target);
    }
    if (have_old) {
        for (uint32_t index = 0; index < old.count; index++) {
            struct ext4_xattr *item = &old.items[index];
            if (!item->value_inum) continue;
            int kept = 0;
            for (uint32_t at = 0; at < count && !kept; at++)
                kept = set->items[at].value_inum == item->value_inum;
            if (!kept) ea_inode_deref(item->value_inum);
        }
    }
    status = inode_write(ino, inode);

done:
    if (have_old) xattr_set_free(&old);
    kfree(order);
    kfree(inside);
    kfree(outside);
    kfree(image);
    return status;
}

void xattr_release(uint32_t ino, struct ext4_inode *inode) {
    struct ext4_xattr_set set;
    if (xattr_load(ino, inode, &set) == 0) {
        for (uint32_t index = 0; index < set.count; index++)
            if (set.items[index].value_inum) ea_inode_deref(set.items[index].value_inum);
        xattr_set_free(&set);
    }
    uint64_t block = inode_file_acl(inode);
    if (block && block < fs->blocks_count) block_deref(inode, block);
    inode_set_file_acl(inode, 0);
}

static const struct {
    uint8_t index;
    const char *prefix;
} prefixes[] = {
    {EXT4_XATTR_INDEX_USER, "user."},
    {EXT4_XATTR_INDEX_TRUSTED, "trusted."},
    {EXT4_XATTR_INDEX_SECURITY, "security."},
};

static int64_t acl_to_abi(const uint8_t *disk, uint32_t size, uint8_t *out, uint32_t capacity) {
    if (size < 4U || get32(disk) != ACL_DISK_VERSION) return -1;
    uint32_t at = 4U;
    uint32_t written = 4U;
    if (capacity < 4U) return -1;
    put32(out, ACL_ABI_VERSION);
    while (at + 4U <= size) {
        uint16_t tag = get16(disk + at);
        uint16_t perm = get16(disk + at + 2U);
        uint32_t id = ACL_NO_ID;
        at += 4U;
        if (tag == ACL_USER || tag == ACL_GROUP) {
            if (at + 4U > size) return -1;
            id = get32(disk + at);
            at += 4U;
        }
        if (written + 8U > capacity) return -1;
        put16(out + written, tag);
        put16(out + written + 2U, perm);
        put32(out + written + 4U, id);
        written += 8U;
    }
    return written;
}

static int64_t abi_to_acl(const uint8_t *abi, uint32_t size, uint8_t *out, uint32_t capacity) {
    if (size < 4U || get32(abi) != ACL_ABI_VERSION || (size - 4U) % 8U) return -1;
    if (capacity < 4U) return -1;
    put32(out, ACL_DISK_VERSION);
    uint32_t written = 4U;
    for (uint32_t at = 4U; at < size; at += 8U) {
        uint16_t tag = get16(abi + at);
        uint32_t need = tag == ACL_USER || tag == ACL_GROUP ? 8U : 4U;
        if (written + need > capacity) return -1;
        put16(out + written, tag);
        put16(out + written + 2U, get16(abi + at + 2U));
        if (need == 8U) put32(out + written + 4U, get32(abi + at + 4U));
        written += need;
    }
    return written;
}

int xattr_to_vfs(struct vfs_node *node, const struct ext4_xattr_set *set) {
    char name[VFS_XATTR_NAME_MAX + 1U];
    for (uint32_t index = 0; index < set->count; index++) {
        const struct ext4_xattr *item = &set->items[index];
        if (item->index == EXT4_XATTR_INDEX_ACL_ACCESS && !item->name_len && item->value) {
            uint8_t abi[4U + 64U * 8U];
            int64_t length = acl_to_abi(item->value, item->value_size, abi, sizeof(abi));
            if (length > 0) (void)acl_import(node, abi, (size_t)length);
            continue;
        }
        for (size_t at = 0; at < sizeof(prefixes) / sizeof(prefixes[0]); at++) {
            if (prefixes[at].index != item->index) continue;
            size_t prefix = strlen(prefixes[at].prefix);
            if (prefix + item->name_len > VFS_XATTR_NAME_MAX) break;
            memcpy(name, prefixes[at].prefix, prefix);
            memcpy(name + prefix, item->name, item->name_len);
            name[prefix + item->name_len] = '\0';
            (void)vfs_xattr_import(node, name, item->value, item->value_size);
            break;
        }
    }
    return 0;
}

static int managed_index(uint8_t index) {
    return index == EXT4_XATTR_INDEX_USER || index == EXT4_XATTR_INDEX_ACL_ACCESS ||
        index == EXT4_XATTR_INDEX_TRUSTED || index == EXT4_XATTR_INDEX_SECURITY;
}

static int vfs_still_has(const struct vfs_node *node, const struct ext4_xattr *item) {
    for (size_t at = 0; at < sizeof(prefixes) / sizeof(prefixes[0]); at++) {
        if (prefixes[at].index != item->index) continue;
        size_t prefix = strlen(prefixes[at].prefix);
        for (const struct vfs_xattr *attr = vfs_xattr_first(node); attr; attr = attr->next)
            if (strncmp(attr->name, prefixes[at].prefix, prefix) == 0 &&
                strlen(attr->name + prefix) == item->name_len &&
                memcmp(attr->name + prefix, item->name, item->name_len) == 0 &&
                attr->size == item->value_size &&
                (!attr->size || memcmp(attr->value, item->value, attr->size) == 0))
                return 1;
    }
    return 0;
}

int xattr_from_vfs(const struct vfs_node *node, struct ext4_xattr_set *set) {
    for (uint32_t index = 0; index < set->count;) {
        struct ext4_xattr *item = &set->items[index];
        if (managed_index(item->index) && (!item->value_inum || !vfs_still_has(node, item))) {
            kfree(item->value);
            memmove(item, item + 1, (set->count - index - 1U) * sizeof(*item));
            set->count--;
            continue;
        }
        index++;
    }
    uint8_t abi[4U + 72U * 8U];
    int64_t length = acl_xattr_get((struct vfs_node *)node, ACL_XATTR_ACCESS, abi, sizeof(abi));
    if (length > 0) {
        uint8_t disk[4U + 72U * 8U];
        int64_t stored = abi_to_acl(abi, (uint32_t)length, disk, sizeof(disk));
        if (stored > 0 &&
            xattr_set_put(set, EXT4_XATTR_INDEX_ACL_ACCESS, "", disk, (uint32_t)stored) != 0)
            return -1;
    }
    for (const struct vfs_xattr *attr = vfs_xattr_first(node); attr; attr = attr->next) {
        for (size_t at = 0; at < sizeof(prefixes) / sizeof(prefixes[0]); at++) {
            size_t prefix = strlen(prefixes[at].prefix);
            if (strncmp(attr->name, prefixes[at].prefix, prefix) != 0) continue;
            if (xattr_set_find(set, prefixes[at].index, attr->name + prefix)) break;
            if (xattr_set_put(set, prefixes[at].index, attr->name + prefix, attr->value,
                              attr->size) != 0)
                return -1;
            break;
        }
    }
    return 0;
}
