#include <stddef.h>
#include <stdint.h>
#include "../include/block.h"
#include "../include/fatfs.h"
#include "../include/heap.h"
#include "../include/kstring.h"
#include "../include/time.h"
#include "../include/vfs.h"

static void vfs_guard_release(int *unused) {
    (void)unused;
    vfs_lock_release();
}

#define VFS_GUARD \
    __attribute__((cleanup(vfs_guard_release))) int vfs_guard = (vfs_lock_acquire(), 0)

extern void kprintf(const char *fmt, ...);

#define EINVAL 22
#define ENODEV 19
#define ENOMEM 12
#define EIO 5
#define ENOSPC 28

#define FAT_ATTR_READ_ONLY 0x01U
#define FAT_ATTR_HIDDEN 0x02U
#define FAT_ATTR_SYSTEM 0x04U
#define FAT_ATTR_VOLUME_ID 0x08U
#define FAT_ATTR_DIRECTORY 0x10U
#define FAT_ATTR_LONG_NAME 0x0FU

#define FAT_ENTRY_FREE 0xE5U
#define FAT_ENTRY_END 0x00U
#define FAT_MAX_NAME 255

struct fat_volume {
    int used;
    const struct block_device *device;
    uint32_t bytes_per_sector;
    uint32_t sectors_per_cluster;
    uint32_t cluster_bytes;
    uint32_t fat_start;
    uint32_t fat_sectors;
    uint32_t fat_count;
    uint32_t root_start;
    uint32_t root_sectors;
    uint32_t data_start;
    uint32_t cluster_count;
    uint32_t root_cluster;
    int bits;
    struct vfs_node *root;
};

struct fat_file {
    struct fat_volume *volume;
    uint32_t first_cluster;

    uint32_t entry_cluster;
    uint32_t entry_offset;
};

static struct fat_volume **volumes;
static int volume_capacity;

static int read_sectors(struct fat_volume *volume, uint64_t sector,
                        uint32_t count, void *out) {
    return volume->device->read(volume->device->context, sector, count, out);
}

static int write_sectors(struct fat_volume *volume, uint64_t sector,
                         uint32_t count, const void *in) {
    if (!volume->device->write) return -1;
    return volume->device->write(volume->device->context, sector, count, in);
}

static uint64_t cluster_sector(struct fat_volume *volume, uint32_t cluster) {
    return (uint64_t)volume->data_start +
           (uint64_t)(cluster - 2U) * volume->sectors_per_cluster;
}

static int cluster_is_end(struct fat_volume *volume, uint32_t value) {
    if (volume->bits == 12) return value >= 0x0FF8U;
    if (volume->bits == 16) return value >= 0xFFF8U;
    return value >= 0x0FFFFFF8U;
}

static int fat_entry_read(struct fat_volume *volume, uint32_t cluster, uint32_t *out) {
    uint8_t sector[BLOCK_SECTOR_SIZE * 2];
    uint64_t offset;
    uint32_t width;

    if (volume->bits == 12) {
        offset = (uint64_t)cluster + cluster / 2U;
        width = 2;
    } else if (volume->bits == 16) {
        offset = (uint64_t)cluster * 2U;
        width = 2;
    } else {
        offset = (uint64_t)cluster * 4U;
        width = 4;
    }

    uint64_t sector_index = offset / volume->bytes_per_sector;
    uint32_t within = (uint32_t)(offset % volume->bytes_per_sector);

    if (read_sectors(volume, volume->fat_start + sector_index, 2, sector) != 0)
        return -1;

    uint32_t value = 0;
    for (uint32_t index = 0; index < width; index++)
        value |= (uint32_t)sector[within + index] << (index * 8U);

    if (volume->bits == 12)
        value = (cluster & 1U) ? (value >> 4) : (value & 0x0FFFU);
    else if (volume->bits == 32)
        value &= 0x0FFFFFFFU;

    *out = value;
    return 0;
}

static int fat_entry_write(struct fat_volume *volume, uint32_t cluster, uint32_t value) {
    uint8_t sector[BLOCK_SECTOR_SIZE * 2];
    uint64_t offset;

    if (volume->bits == 12) offset = (uint64_t)cluster + cluster / 2U;
    else if (volume->bits == 16) offset = (uint64_t)cluster * 2U;
    else offset = (uint64_t)cluster * 4U;

    uint64_t sector_index = offset / volume->bytes_per_sector;
    uint32_t within = (uint32_t)(offset % volume->bytes_per_sector);

    for (uint32_t copy = 0; copy < volume->fat_count; copy++) {
        uint64_t base = volume->fat_start + (uint64_t)copy * volume->fat_sectors;
        if (read_sectors(volume, base + sector_index, 2, sector) != 0) return -1;

        if (volume->bits == 12) {
            uint16_t packed = (uint16_t)(sector[within] | (sector[within + 1] << 8));
            if (cluster & 1U) packed = (uint16_t)((packed & 0x000FU) | (value << 4));
            else packed = (uint16_t)((packed & 0xF000U) | (value & 0x0FFFU));
            sector[within] = (uint8_t)packed;
            sector[within + 1] = (uint8_t)(packed >> 8);
        } else if (volume->bits == 16) {
            sector[within] = (uint8_t)value;
            sector[within + 1] = (uint8_t)(value >> 8);
        } else {
            uint32_t existing = 0;
            for (uint32_t index = 0; index < 4U; index++)
                existing |= (uint32_t)sector[within + index] << (index * 8U);

            uint32_t merged = (existing & 0xF0000000U) | (value & 0x0FFFFFFFU);
            for (uint32_t index = 0; index < 4U; index++)
                sector[within + index] = (uint8_t)(merged >> (index * 8U));
        }

        if (write_sectors(volume, base + sector_index, 2, sector) != 0) return -1;
    }
    return 0;
}

static uint32_t fat_allocate_cluster(struct fat_volume *volume) {
    for (uint32_t cluster = 2U; cluster < volume->cluster_count + 2U; cluster++) {
        uint32_t value = 0;
        if (fat_entry_read(volume, cluster, &value) != 0) return 0;
        if (value) continue;
        uint32_t end = volume->bits == 32 ? 0x0FFFFFFFU :
                       (volume->bits == 16 ? 0xFFFFU : 0x0FFFU);
        if (fat_entry_write(volume, cluster, end) != 0) return 0;
        return cluster;
    }
    return 0;
}

static uint32_t cluster_at(struct fat_volume *volume, uint32_t first,
                           uint64_t offset, int grow) {
    if (first < 2U) return 0;
    uint32_t cluster = first;
    uint64_t steps = offset / volume->cluster_bytes;
    while (steps--) {
        uint32_t next = 0;
        if (fat_entry_read(volume, cluster, &next) != 0) return 0;
        if (cluster_is_end(volume, next)) {
            if (!grow) return 0;
            next = fat_allocate_cluster(volume);
            if (!next) return 0;
            if (fat_entry_write(volume, cluster, next) != 0) return 0;
        }
        cluster = next;
    }
    return cluster;
}

static int64_t fat_node_read(struct vfs_node *node, uint64_t offset,
                             size_t size, void *buffer) {
    VFS_GUARD;
    struct fat_file *file = (struct fat_file *)node->fs_private;
    if (!file) return -1;
    struct fat_volume *volume = file->volume;
    if (offset >= node->length) return 0;
    if (size > node->length - offset) size = (size_t)(node->length - offset);

    uint8_t *out = (uint8_t *)buffer;
    size_t moved = 0;
    uint8_t staging[BLOCK_SECTOR_SIZE];

    while (moved < size) {
        uint32_t cluster = cluster_at(volume, file->first_cluster, offset, 0);
        if (!cluster) break;
        uint32_t within = (uint32_t)(offset % volume->cluster_bytes);
        uint64_t sector = cluster_sector(volume, cluster) + within / volume->bytes_per_sector;
        uint32_t sector_offset = within % volume->bytes_per_sector;

        if (read_sectors(volume, sector, 1, staging) != 0) return -1;
        size_t chunk = volume->bytes_per_sector - sector_offset;
        if (chunk > size - moved) chunk = size - moved;
        memcpy(out + moved, staging + sector_offset, chunk);
        moved += chunk;
        offset += chunk;
    }
    return (int64_t)moved;
}

static int fat_write_directory_entry(struct fat_file *file, uint32_t size,
                                     uint32_t first_cluster);

static int release_chain(struct fat_volume *volume, uint32_t cluster) {
    while (cluster >= 2U && !cluster_is_end(volume, cluster)) {
        uint32_t next = 0;
        if (fat_entry_read(volume, cluster, &next) != 0) return -1;
        if (fat_entry_write(volume, cluster, 0) != 0) return -1;
        cluster = next;
    }
    return 0;
}

static int fat_node_truncate(struct vfs_node *node, uint64_t length) {
    VFS_GUARD;
    struct fat_file *file = (struct fat_file *)node->fs_private;
    if (!file) return -1;
    struct fat_volume *volume = file->volume;
    if (!volume->device->write) return -1;
    if (length > 0xFFFFFFFFULL) return -1;

    if (length >= node->length) {
        return fat_write_directory_entry(file, (uint32_t)length, file->first_cluster);
    }

    uint32_t first = file->first_cluster;
    if (!length) {
        if (release_chain(volume, first) != 0) return -1;
        file->first_cluster = 0;
        return fat_write_directory_entry(file, 0, 0);
    }

    uint32_t last = cluster_at(volume, first, length - 1U, 0);
    if (!last) return -1;
    uint32_t next = 0;
    if (fat_entry_read(volume, last, &next) != 0) return -1;
    uint32_t end = volume->bits == 32 ? 0x0FFFFFFFU :
                   (volume->bits == 16 ? 0xFFFFU : 0x0FFFU);
    if (fat_entry_write(volume, last, end) != 0) return -1;
    if (!cluster_is_end(volume, next) && release_chain(volume, next) != 0) return -1;
    return fat_write_directory_entry(file, (uint32_t)length, first);
}

static int64_t fat_node_write(struct vfs_node *node, uint64_t offset,
                              size_t size, const void *buffer) {
    VFS_GUARD;
    struct fat_file *file = (struct fat_file *)node->fs_private;
    if (!file) return -1;
    struct fat_volume *volume = file->volume;
    if (!volume->device->write) return -1;

    const uint8_t *in = (const uint8_t *)buffer;
    size_t moved = 0;
    uint8_t staging[BLOCK_SECTOR_SIZE];

    if (file->first_cluster < 2U && size) {
        uint32_t cluster = fat_allocate_cluster(volume);
        if (!cluster) return -1;
        file->first_cluster = cluster;
    }

    while (moved < size) {
        uint32_t cluster = cluster_at(volume, file->first_cluster, offset, 1);
        if (!cluster) break;
        uint32_t within = (uint32_t)(offset % volume->cluster_bytes);
        uint64_t sector = cluster_sector(volume, cluster) + within / volume->bytes_per_sector;
        uint32_t sector_offset = within % volume->bytes_per_sector;

        size_t chunk = volume->bytes_per_sector - sector_offset;
        if (chunk > size - moved) chunk = size - moved;

        if (chunk != volume->bytes_per_sector) {
            if (read_sectors(volume, sector, 1, staging) != 0) return -1;
        }
        memcpy(staging + sector_offset, in + moved, chunk);
        if (write_sectors(volume, sector, 1, staging) != 0) return -1;

        moved += chunk;
        offset += chunk;
    }

    if (offset > node->length) node->length = offset;
    if (fat_write_directory_entry(file, (uint32_t)node->length,
                                  file->first_cluster) != 0) return -1;
    return (int64_t)moved;
}

static int directory_read(struct fat_volume *volume, uint32_t cluster,
                          uint64_t offset, uint32_t count, void *out) {
    if (!cluster) {
        uint64_t sector = volume->root_start + offset / volume->bytes_per_sector;
        if (offset / volume->bytes_per_sector >= volume->root_sectors) return -1;
        return read_sectors(volume, sector, count, out);
    }
    uint32_t at = cluster_at(volume, cluster, offset, 0);
    if (!at) return -1;
    uint32_t within = (uint32_t)(offset % volume->cluster_bytes);
    return read_sectors(volume, cluster_sector(volume, at) + within / volume->bytes_per_sector,
                        count, out);
}

static int directory_write(struct fat_volume *volume, uint32_t cluster,
                           uint64_t offset, uint32_t count, const void *in) {
    if (!cluster) {
        uint64_t sector = volume->root_start + offset / volume->bytes_per_sector;
        if (offset / volume->bytes_per_sector >= volume->root_sectors) return -1;
        return write_sectors(volume, sector, count, in);
    }
    uint32_t at = cluster_at(volume, cluster, offset, 0);
    if (!at) return -1;
    uint32_t within = (uint32_t)(offset % volume->cluster_bytes);
    return write_sectors(volume, cluster_sector(volume, at) + within / volume->bytes_per_sector,
                         count, in);
}

static int fat_write_directory_entry(struct fat_file *file, uint32_t size,
                                     uint32_t first_cluster) {
    struct fat_volume *volume = file->volume;
    uint64_t sector_offset = file->entry_offset -
                             (file->entry_offset % volume->bytes_per_sector);
    uint32_t within = file->entry_offset % volume->bytes_per_sector;
    uint8_t sector[BLOCK_SECTOR_SIZE];

    if (directory_read(volume, file->entry_cluster, sector_offset, 1, sector) != 0)
        return -1;
    uint8_t *entry = sector + within;
    entry[26] = (uint8_t)first_cluster;
    entry[27] = (uint8_t)(first_cluster >> 8);
    entry[20] = (uint8_t)(first_cluster >> 16);
    entry[21] = (uint8_t)(first_cluster >> 24);
    for (uint32_t index = 0; index < 4U; index++)
        entry[28 + index] = (uint8_t)(size >> (index * 8U));
    return directory_write(volume, file->entry_cluster, sector_offset, 1, sector);
}

static void short_name(const uint8_t *entry, char *out) {
    size_t length = 0;
    for (int index = 0; index < 8; index++) {
        if (entry[index] == ' ') break;
        out[length++] = (char)entry[index];
    }
    if (entry[8] != ' ') {
        out[length++] = '.';
        for (int index = 8; index < 11; index++) {
            if (entry[index] == ' ') break;
            out[length++] = (char)entry[index];
        }
    }
    out[length] = '\0';

    int upper = 1;
    for (size_t index = 0; index < length; index++)
        if (out[index] >= 'a' && out[index] <= 'z') upper = 0;
    if (upper)
        for (size_t index = 0; index < length; index++)
            if (out[index] >= 'A' && out[index] <= 'Z') out[index] += 32;
}

static void long_name_piece(const uint8_t *entry, char *out) {
    static const int offsets[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
    for (int index = 0; index < 13; index++) {
        uint16_t unit = (uint16_t)(entry[offsets[index]] |
                                   (entry[offsets[index] + 1] << 8));
        if (unit == 0 || unit == 0xFFFFU) { out[index] = '\0'; return; }
        out[index] = unit < 0x100U ? (char)unit : '_';
    }
    out[13] = '\0';
}

static struct vfs_node *build_directory(struct fat_volume *volume,
                                        uint32_t cluster, const char *name,
                                        struct vfs_node *parent, int depth);
static int fat_adopt_child(struct vfs_node *directory, struct vfs_node *child);

static int attach_entry(struct fat_volume *volume, struct vfs_node *directory,
                        const uint8_t *entry, const char *name,
                        uint32_t entry_cluster, uint32_t entry_offset, int depth) {
    uint32_t first = (uint32_t)(entry[26] | (entry[27] << 8)) |
                     ((uint32_t)(entry[20] | (entry[21] << 8)) << 16);
    uint32_t size = 0;
    for (uint32_t index = 0; index < 4U; index++)
        size |= (uint32_t)entry[28 + index] << (index * 8U);

    if (entry[11] & FAT_ATTR_DIRECTORY) {
        struct vfs_node *child = build_directory(volume, first, name, directory, depth + 1);
        return child ? 0 : -1;
    }

    struct vfs_node *node = vfs_alloc_node(name, VFS_FILE);
    if (!node) return -1;
    struct fat_file *file = (struct fat_file *)kmalloc(sizeof(*file));
    if (!file) return -1;
    file->volume = volume;
    file->first_cluster = first;
    file->entry_cluster = entry_cluster;
    file->entry_offset = entry_offset;

    node->fs_private = file;
    node->length = size;
    node->mode = (entry[11] & FAT_ATTR_READ_ONLY) ? 0444U : 0644U;
    node->read = fat_node_read;
    node->write = fat_node_write;
    node->truncate = fat_node_truncate;
    vfs_attach(directory, node);
    return 0;
}

static struct vfs_node *build_directory(struct fat_volume *volume, uint32_t cluster,
                                        const char *name, struct vfs_node *parent,
                                        int depth) {
    if (depth > 24) return NULL;

    struct vfs_node *directory = vfs_alloc_node(name, VFS_DIRECTORY);
    if (!directory) return NULL;
    directory->mode = 0755U;
    directory->adopt = fat_adopt_child;
    struct fat_file *self = (struct fat_file *)kmalloc(sizeof(*self));
    if (self) {
        self->volume = volume;
        self->first_cluster = cluster;
        self->entry_cluster = cluster;
        self->entry_offset = 0;
        directory->fs_private = self;
    }
    if (parent) vfs_attach(parent, directory);

    uint8_t sector[BLOCK_SECTOR_SIZE];
    char assembled[FAT_MAX_NAME + 16];
    int have_long = 0;
    assembled[0] = '\0';

    uint64_t offset = 0;
    uint64_t limit = cluster ? (uint64_t)volume->cluster_count * volume->cluster_bytes
                             : (uint64_t)volume->root_sectors * volume->bytes_per_sector;

    while (offset < limit) {
        if (directory_read(volume, cluster, offset, 1, sector) != 0) break;
        for (uint32_t at = 0; at + 32U <= volume->bytes_per_sector; at += 32U) {
            const uint8_t *entry = sector + at;
            if (entry[0] == FAT_ENTRY_END) return directory;
            if (entry[0] == FAT_ENTRY_FREE) { have_long = 0; continue; }

            if ((entry[11] & FAT_ATTR_LONG_NAME) == FAT_ATTR_LONG_NAME) {
                char piece[16];
                long_name_piece(entry, piece);
                char merged[FAT_MAX_NAME + 16];
                size_t piece_length = strlen(piece);
                size_t have = strlen(assembled);
                if (piece_length + have < sizeof(merged) - 1U) {
                    memcpy(merged, piece, piece_length);
                    memcpy(merged + piece_length, assembled, have + 1U);
                    memcpy(assembled, merged, piece_length + have + 1U);
                    have_long = 1;
                }
                continue;
            }
            if (entry[11] & FAT_ATTR_VOLUME_ID) { have_long = 0; assembled[0] = '\0'; continue; }

            char name_buffer[16];
            short_name(entry, name_buffer);
            if (name_buffer[0] == '.') { have_long = 0; assembled[0] = '\0'; continue; }

            const char *use = have_long && assembled[0] ? assembled : name_buffer;
            (void)attach_entry(volume, directory, entry, use, cluster, (uint32_t)(offset + at), depth);
            have_long = 0;
            assembled[0] = '\0';
        }
        offset += volume->bytes_per_sector;
    }
    return directory;
}

static void make_short_name(const char *name, uint8_t *out) {
    memset(out, ' ', 11);
    const char *dot = NULL;
    for (const char *at = name; *at; at++) if (*at == '.') dot = at;

    size_t index = 0;
    for (const char *at = name; *at && index < 8U; at++) {
        if (at == dot) break;
        char value = *at;
        if (value == ' ' || value == '.') continue;
        if (value >= 'a' && value <= 'z') value = (char)(value - 32);
        out[index++] = (uint8_t)value;
    }
    if (!index) out[0] = (uint8_t)'_';

    if (dot) {
        size_t extension = 0;
        for (const char *at = dot + 1; *at && extension < 3U; at++) {
            char value = *at;
            if (value >= 'a' && value <= 'z') value = (char)(value - 32);
            out[8 + extension++] = (uint8_t)value;
        }
    }
}

static void entry_timestamp(uint16_t *date_out, uint16_t *time_out) {
    struct tunix_rtc_time now;
    if (time_get_rtc(&now) != 0 || now.year < 1980) {
        *date_out = (uint16_t)((1U << 5) | 1U);
        *time_out = 0;
        return;
    }
    *date_out = (uint16_t)((((uint32_t)now.year - 1980U) << 9) |
                           ((uint32_t)now.month << 5) | (uint32_t)now.day);
    *time_out = (uint16_t)(((uint32_t)now.hour << 11) |
                           ((uint32_t)now.minute << 5) | ((uint32_t)now.second / 2U));
}

static int find_free_entry(struct fat_volume *volume, uint32_t cluster,
                           uint64_t *offset_out) {
    uint8_t sector[BLOCK_SECTOR_SIZE];
    uint64_t offset = 0;
    uint64_t limit = cluster ? (uint64_t)volume->cluster_count * volume->cluster_bytes
                             : (uint64_t)volume->root_sectors * volume->bytes_per_sector;

    while (offset < limit) {
        if (directory_read(volume, cluster, offset, 1, sector) != 0) break;
        for (uint32_t at = 0; at + 32U <= volume->bytes_per_sector; at += 32U) {
            if (sector[at] == FAT_ENTRY_END || sector[at] == FAT_ENTRY_FREE) {
                *offset_out = offset + at;
                return 0;
            }
        }
        offset += volume->bytes_per_sector;
    }
    return -1;
}

static int fat_adopt_child(struct vfs_node *directory, struct vfs_node *child) {
    struct fat_file *parent = (struct fat_file *)directory->fs_private;
    if (!parent || !child) return -1;
    if (child->fs_private) return 0;
    if ((child->flags & 0xFFU) != VFS_FILE) return -1;
    struct fat_volume *volume = parent->volume;
    if (!volume->device->write) return -1;

    uint64_t offset = 0;
    if (find_free_entry(volume, parent->first_cluster, &offset) != 0) return -1;

    uint64_t sector_offset = offset - (offset % volume->bytes_per_sector);
    uint32_t within = (uint32_t)(offset % volume->bytes_per_sector);
    uint8_t sector[BLOCK_SECTOR_SIZE];
    if (directory_read(volume, parent->first_cluster, sector_offset, 1, sector) != 0)
        return -1;

    uint8_t *entry = sector + within;
    int was_last = entry[0] == FAT_ENTRY_END;
    memset(entry, 0, 32);
    make_short_name(child->name, entry);
    entry[11] = 0;
    uint16_t date = 0, stamp = 0;
    entry_timestamp(&date, &stamp);
    entry[14] = (uint8_t)stamp;
    entry[15] = (uint8_t)(stamp >> 8);
    entry[16] = (uint8_t)date;
    entry[17] = (uint8_t)(date >> 8);
    entry[18] = (uint8_t)date;
    entry[19] = (uint8_t)(date >> 8);
    entry[22] = (uint8_t)stamp;
    entry[23] = (uint8_t)(stamp >> 8);
    entry[24] = (uint8_t)date;
    entry[25] = (uint8_t)(date >> 8);
    if (directory_write(volume, parent->first_cluster, sector_offset, 1, sector) != 0)
        return -1;

    if (was_last && within + 64U <= volume->bytes_per_sector) {
        sector[within + 32U] = FAT_ENTRY_END;
        if (directory_write(volume, parent->first_cluster, sector_offset, 1, sector) != 0)
            return -1;
    }

    struct fat_file *file = (struct fat_file *)kmalloc(sizeof(*file));
    if (!file) return -1;
    file->volume = volume;
    file->first_cluster = 0;
    file->entry_cluster = parent->first_cluster;
    file->entry_offset = (uint32_t)offset;

    child->fs_private = file;
    child->length = 0;
    child->read = fat_node_read;
    child->write = fat_node_write;
    child->truncate = fat_node_truncate;
    return 0;
}

static const struct block_device *device_from_source(const char *source) {
    if (!source) return NULL;
    int index = block_device_index_by_name(source);
    return index < 0 ? NULL : block_device_at(index);
}

int fatfs_mount(const char *source, const char *mount_name, struct vfs_node **root_out) {
    VFS_GUARD;
    const struct block_device *device = device_from_source(source);
    if (!device) return -ENODEV;

    struct fat_volume *volume = NULL;
    int slot = 0;
    while (slot < volume_capacity && volumes[slot] && volumes[slot]->used) slot++;
    if (slot == volume_capacity) {
        int capacity = volume_capacity ? volume_capacity * 2 : 4;
        struct fat_volume **grown = kmalloc((size_t)capacity * sizeof(*grown));
        if (!grown) return -ENOSPC;
        memset(grown, 0, (size_t)capacity * sizeof(*grown));
        if (volume_capacity) memcpy(grown, volumes, (size_t)volume_capacity * sizeof(*grown));
        kfree(volumes);
        volumes = grown;
        volume_capacity = capacity;
    }
    if (!volumes[slot]) {
        volumes[slot] = kmalloc(sizeof(struct fat_volume));
        if (!volumes[slot]) return -ENOSPC;
        memset(volumes[slot], 0, sizeof(struct fat_volume));
    }
    volume = volumes[slot];

    uint8_t boot[BLOCK_SECTOR_SIZE];
    if (device->read(device->context, 0, 1, boot) != 0) return -EIO;
    if (boot[510] != 0x55U || boot[511] != 0xAAU) return -EINVAL;

    memset(volume, 0, sizeof(*volume));
    volume->device = device;
    volume->bytes_per_sector = (uint32_t)(boot[11] | (boot[12] << 8));
    volume->sectors_per_cluster = boot[13];
    uint32_t reserved = (uint32_t)(boot[14] | (boot[15] << 8));
    volume->fat_count = boot[16];
    uint32_t root_entries = (uint32_t)(boot[17] | (boot[18] << 8));
    uint32_t total_short = (uint32_t)(boot[19] | (boot[20] << 8));
    uint32_t fat_short = (uint32_t)(boot[22] | (boot[23] << 8));
    uint32_t total_long = (uint32_t)(boot[32] | (boot[33] << 8) |
                                     (boot[34] << 16) | (boot[35] << 24));
    uint32_t fat_long = (uint32_t)(boot[36] | (boot[37] << 8) |
                                   (boot[38] << 16) | (boot[39] << 24));

    if (volume->bytes_per_sector != BLOCK_SECTOR_SIZE) return -EINVAL;
    if (!volume->sectors_per_cluster || !volume->fat_count || !reserved) return -EINVAL;

    volume->fat_sectors = fat_short ? fat_short : fat_long;
    uint32_t total = total_short ? total_short : total_long;
    if (!volume->fat_sectors || !total) return -EINVAL;

    volume->cluster_bytes = volume->bytes_per_sector * volume->sectors_per_cluster;
    volume->fat_start = reserved;
    volume->root_start = reserved + volume->fat_count * volume->fat_sectors;
    volume->root_sectors = (root_entries * 32U + volume->bytes_per_sector - 1U) /
                           volume->bytes_per_sector;
    volume->data_start = volume->root_start + volume->root_sectors;
    if (total <= volume->data_start) return -EINVAL;
    volume->cluster_count = (total - volume->data_start) / volume->sectors_per_cluster;

    if (volume->cluster_count < 4085U) volume->bits = 12;
    else if (volume->cluster_count < 65525U) volume->bits = 16;
    else volume->bits = 32;

    if (volume->bits == 32) {
        volume->root_cluster = (uint32_t)(boot[44] | (boot[45] << 8) |
                                          (boot[46] << 16) | (boot[47] << 24));
        volume->root_sectors = 0;
        volume->root_start = 0;
        volume->data_start = reserved + volume->fat_count * volume->fat_sectors;
        volume->cluster_count = (total - volume->data_start) / volume->sectors_per_cluster;
        if (volume->root_cluster < 2U) return -EINVAL;
    }

    volume->used = 1;
    struct vfs_node *root = build_directory(volume,
                                            volume->bits == 32 ? volume->root_cluster : 0U,
                                            mount_name, NULL, 0);
    if (!root) {
        volume->used = 0;
        return -EIO;
    }
    volume->root = root;
    *root_out = root;
    kprintf("FAT%d: %s, %u clusters of %u bytes\n", volume->bits, source,
            (unsigned)volume->cluster_count, (unsigned)volume->cluster_bytes);
    return 0;
}

int fatfs_owns(const struct vfs_node *node) {
    VFS_GUARD;
    if (!node) return 0;
    for (int index = 0; index < volume_capacity; index++)
        if (volumes[index] && volumes[index]->used && volumes[index]->root == node) return 1;
    return 0;
}

static void free_subtree(struct vfs_node *node) {
    if (!node) return;
    struct vfs_node *child = node->children;
    while (child) {
        struct vfs_node *next = child->next;
        free_subtree(child);
        child = next;
    }
    if (node->fs_private) kfree(node->fs_private);
    node->fs_private = NULL;
}

void fatfs_unmount(struct vfs_node *root) {
    VFS_GUARD;
    for (int index = 0; index < volume_capacity; index++) {
        if (!volumes[index] || !volumes[index]->used || volumes[index]->root != root) continue;
        free_subtree(root);
        volumes[index]->used = 0;
        volumes[index]->root = NULL;
        return;
    }
}
