#include <stddef.h>
#include <stdint.h>

#include "arch.h"

#define EXT2_MAGIC     0xEF53U
#define EXT2_ROOT_INO  2U
#define SECTOR_BYTES   512U
#define BLOCK_BYTES    4096U

// Every field is read at its byte offset, so nothing here depends on how the
// compiler would lay out a packed struct.
#define SB_LOG_BLOCK_SIZE   24
#define SB_FIRST_DATA_BLOCK 20
#define SB_BLOCKS_PER_GROUP 32
#define SB_INODES_PER_GROUP 40
#define SB_MAGIC            56
#define SB_INODE_SIZE       88

#define GD_INODE_TABLE 8
#define GD_BYTES       32

#define INODE_MODE   0
#define INODE_SIZE   4
#define INODE_BLOCK  40

#define DIRENT_INODE     0
#define DIRENT_REC_LEN   4
#define DIRENT_NAME_LEN  6
#define DIRENT_NAME      8

static uint32_t block_size;
static uint32_t inode_size;
static uint32_t inodes_per_group;
static uint32_t group_desc_block;
static int mounted;

static uint8_t group_scratch[BLOCK_BYTES];
static uint8_t indirect_scratch[BLOCK_BYTES];
static uint8_t data_scratch[BLOCK_BYTES];
static uint8_t inode_scratch[256];

static uint16_t le16(const uint8_t *p) {
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int read_block(uint32_t block, uint8_t *out) {
    uint32_t sectors = block_size / SECTOR_BYTES;
    for (uint32_t i = 0; i < sectors; i++)
        if (virtio_blk_read((uint64_t)block * sectors + i, out + i * SECTOR_BYTES) != 0)
            return -1;
    return 0;
}

static int read_inode(uint32_t ino, uint8_t *out) {
    if (!ino) return -1;
    uint32_t group = (ino - 1) / inodes_per_group;
    uint32_t index = (ino - 1) % inodes_per_group;

    uint32_t per_block = block_size / GD_BYTES;
    if (read_block(group_desc_block + group / per_block, group_scratch) != 0) return -1;
    uint32_t table = le32(group_scratch + (group % per_block) * GD_BYTES + GD_INODE_TABLE);

    uint32_t byte = index * inode_size;
    if (read_block(table + byte / block_size, group_scratch) != 0) return -1;

    const uint8_t *source = group_scratch + byte % block_size;
    for (uint32_t i = 0; i < inode_size; i++) out[i] = source[i];
    return 0;
}

// Direct blocks and one level of indirection, which is all a boot path needs.
static int inode_block_at(const uint8_t *inode, uint32_t index, uint32_t *out) {
    if (index < 12) {
        *out = le32(inode + INODE_BLOCK + index * 4);
        return 0;
    }
    uint32_t indirect = le32(inode + INODE_BLOCK + 12 * 4);
    if (!indirect) return -1;

    index -= 12;
    if (index >= block_size / 4) return -1;
    if (read_block(indirect, indirect_scratch) != 0) return -1;
    *out = le32(indirect_scratch + index * 4);
    return 0;
}

static int lookup_in_directory(const uint8_t *inode, const char *name,
                               uint32_t length, uint32_t *found) {
    uint32_t size = le32(inode + INODE_SIZE);

    for (uint32_t offset = 0; offset < size; offset += block_size) {
        uint32_t block = 0;
        if (inode_block_at(inode, offset / block_size, &block) != 0 || !block) continue;
        if (read_block(block, data_scratch) != 0) return -1;

        uint32_t position = 0;
        while (position + DIRENT_NAME <= block_size) {
            uint32_t entry = le32(data_scratch + position + DIRENT_INODE);
            uint16_t record = le16(data_scratch + position + DIRENT_REC_LEN);
            uint8_t name_length = data_scratch[position + DIRENT_NAME_LEN];
            if (record < DIRENT_NAME) break;

            if (entry && name_length == length) {
                uint32_t i = 0;
                while (i < length &&
                       data_scratch[position + DIRENT_NAME + i] == (uint8_t)name[i]) i++;
                if (i == length) {
                    *found = entry;
                    return 0;
                }
            }
            position += record;
        }
    }
    return -1;
}

int ext2_mount(void) {
    uint8_t superblock[1024];
    if (virtio_blk_read(2, superblock) != 0) return -1;
    if (virtio_blk_read(3, superblock + SECTOR_BYTES) != 0) return -1;
    if (le16(superblock + SB_MAGIC) != EXT2_MAGIC) return -2;

    block_size = 1024U << le32(superblock + SB_LOG_BLOCK_SIZE);
    if (block_size != BLOCK_BYTES) return -3;          // the x86 driver assumes this too

    inode_size = le16(superblock + SB_INODE_SIZE);
    if (!inode_size || inode_size > sizeof(inode_scratch)) inode_size = 128;
    inodes_per_group = le32(superblock + SB_INODES_PER_GROUP);
    if (!inodes_per_group) return -4;

    group_desc_block = le32(superblock + SB_FIRST_DATA_BLOCK) + 1;
    mounted = 1;
    return 0;
}

int ext2_lookup_path(const char *path, uint32_t *ino_out, uint32_t *size_out) {
    if (!mounted) return -1;

    uint32_t ino = EXT2_ROOT_INO;
    if (read_inode(ino, inode_scratch) != 0) return -1;

    const char *cursor = path;
    while (*cursor == '/') cursor++;
    while (*cursor) {
        const char *start = cursor;
        while (*cursor && *cursor != '/') cursor++;

        uint32_t next = 0;
        if (lookup_in_directory(inode_scratch, start,
                                (uint32_t)(cursor - start), &next) != 0) return -2;
        if (read_inode(next, inode_scratch) != 0) return -1;
        ino = next;

        while (*cursor == '/') cursor++;
    }

    *ino_out = ino;
    *size_out = le32(inode_scratch + INODE_SIZE);
    return 0;
}

int ext2_read_file(uint32_t ino, void *out, uint32_t limit, uint32_t *size_out) {
    if (!mounted) return -1;
    if (read_inode(ino, inode_scratch) != 0) return -1;

    uint32_t size = le32(inode_scratch + INODE_SIZE);
    if (size > limit) return -2;

    uint8_t *target = out;
    for (uint32_t offset = 0; offset < size; offset += block_size) {
        uint32_t block = 0;
        if (inode_block_at(inode_scratch, offset / block_size, &block) != 0) return -3;
        if (!block) return -3;
        if (read_block(block, data_scratch) != 0) return -4;

        uint32_t chunk = size - offset < block_size ? size - offset : block_size;
        for (uint32_t i = 0; i < chunk; i++) target[offset + i] = data_scratch[i];
    }

    *size_out = size;
    return 0;
}
