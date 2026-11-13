#include "ext4.h"

#include <tunix/crc.h>
#include <tunix/ext4.h>
#include <tunix/heap.h>
#include <tunix/jbd2.h>
#include <tunix/kstring.h>
#include <tunix/process.h>
#include <tunix/time.h>

struct ext4_volume *ext4_volumes;
struct ext4_volume *fs;
const char *ext4_failed_stage = "?";
uint8_t ext4_scratch[EXT4_MAX_BLOCK_SIZE];

static uint8_t meta_buf[EXT4_MAX_BLOCK_SIZE] __attribute__((aligned(16)));

struct mutex ext4_io_lock = MUTEX_INITIALIZER("ext4 io", LOCK_RANK_EXT4_IO);
struct mutex ext4_lock = MUTEX_INITIALIZER("ext4", LOCK_RANK_EXT4);
static volatile int commit_behind;

static void vfs_guard_release(int *unused) {
    (void)unused;
    vfs_lock_release();
}

#define VFS_GUARD \
    __attribute__((cleanup(vfs_guard_release))) int vfs_guard = (vfs_lock_acquire(), 0)

static void ext4_guard_release(int *unused) {
    (void)unused;
    mutex_unlock(&ext4_lock);
}

#define EXT4_LOCKED \
    __attribute__((cleanup(ext4_guard_release))) int ext4_guard = (mutex_lock(&ext4_lock), 0)

#define EXT4_COMMIT_INTERVAL_NS (5ULL * 1000ULL * 1000ULL * 1000ULL)
#define EXT4_COMMIT_POLL_NS     (1000ULL * 1000ULL * 1000ULL)

struct ext4_volume *ext4_enter(struct ext4_volume *volume) {
    struct ext4_volume *previous = fs;
    fs = volume;
    return previous;
}

struct ext4_volume *ext4_op_enter(struct ext4_volume *volume) {
    mutex_lock(&ext4_lock);
    return ext4_enter(volume);
}

void ext4_op_leave(struct ext4_volume *saved) {
    if (fs && fs->buffer_count > fs->buffer_limit) buffers_trim();
    fs = saved;
    mutex_unlock(&ext4_lock);
}

struct ext4_volume *ext4_volume_of(const struct vfs_node *node) {
    if (!node || !node->disk_inode || !node->fs_private) return NULL;
    for (struct ext4_volume *volume = ext4_volumes; volume; volume = volume->next)
        if (volume == node->fs_private) return volume;
    return NULL;
}

struct ext4_volume *ext4_active_volume_of(const struct vfs_node *node) {
    struct ext4_volume *volume = ext4_volume_of(node);
    return volume && !volume->loading && !volume->read_only ? volume : NULL;
}

uint32_t ext4_now(void) { return (uint32_t)time_epoch_seconds(); }

void ext4_now_ns(uint32_t *seconds, uint32_t *nanoseconds) {
    uint64_t now = time_realtime_ns();
    *seconds = (uint32_t)(now / 1000000000ULL);
    *nanoseconds = (uint32_t)(now % 1000000000ULL);
}

#define FAILURE_REPORT_LIMIT 8U

void ext4_report_failure(const char *what, const char *name) {
    static unsigned reported;
    if (reported >= FAILURE_REPORT_LIMIT) return;
    reported++;
    kprintf("EXT4: could not %s %s on %s at the %s, %u blocks and %u inodes free%s\n", what, name,
            fs->device->dev_name, ext4_failed_stage, (unsigned)sb_free_blocks(),
            (unsigned)fs->sb.s_free_inodes_count,
            reported == FAILURE_REPORT_LIMIT ? " (last report)" : "");
}

int ext4_volume_read(struct ext4_volume *volume, uint64_t block, uint32_t count, void *out) {
    return block_device_read(volume->device, block * volume->sectors_per_block,
                             count * volume->sectors_per_block, out);
}

int ext4_volume_write(struct ext4_volume *volume, uint64_t block, uint32_t count,
                      const void *data) {
    return block_device_write(volume->device, block * volume->sectors_per_block,
                              count * volume->sectors_per_block, data);
}

int ext4_volume_flush(struct ext4_volume *volume) { return block_device_flush(volume->device); }

uint32_t ext4_csum(uint32_t seed, const void *data, size_t length) {
    return crc32c(seed, data, length);
}

uint32_t ext4_inode_seed(uint32_t ino, uint32_t generation) {
    uint32_t seed = crc32c(fs->csum_seed, &ino, sizeof(ino));
    return crc32c(seed, &generation, sizeof(generation));
}

void ext4_superblock_csum_set(struct ext4_super_block *sb) {
    if (!(sb->s_feature_ro_compat & EXT4_RO_COMPAT_METADATA_CSUM)) return;
    sb->s_checksum = crc32c(0xFFFFFFFFU, sb, offsetof(struct ext4_super_block, s_checksum));
}

static struct ext4_buffer *buffer_of(const uint8_t *data) {
    return (struct ext4_buffer *)(uintptr_t)(data - offsetof(struct ext4_buffer, data));
}

static uint32_t buffer_bucket(uint64_t block) {
    return (uint32_t)((block * 0x9E3779B97F4A7C15ULL) >> 40) & (EXT4_BUFFER_BUCKETS - 1U);
}

static void lru_unlink(struct ext4_buffer *buffer) {
    if (buffer->lru_prev) buffer->lru_prev->lru_next = buffer->lru_next;
    else fs->lru_head = buffer->lru_next;
    if (buffer->lru_next) buffer->lru_next->lru_prev = buffer->lru_prev;
    else fs->lru_tail = buffer->lru_prev;
    buffer->lru_prev = buffer->lru_next = NULL;
}

static void lru_push(struct ext4_buffer *buffer) {
    buffer->lru_prev = NULL;
    buffer->lru_next = fs->lru_head;
    if (fs->lru_head) fs->lru_head->lru_prev = buffer;
    else fs->lru_tail = buffer;
    fs->lru_head = buffer;
}

static struct ext4_buffer *buffer_find(uint64_t block) {
    if (!fs->buffers) return NULL;
    for (struct ext4_buffer *buffer = fs->buffers[buffer_bucket(block)]; buffer;
         buffer = buffer->hash_next)
        if (buffer->block == block) return buffer;
    return NULL;
}

static struct ext4_buffer *buffer_create(uint64_t block) {
    if (!fs->buffers) {
        fs->buffers = (struct ext4_buffer **)kmalloc(EXT4_BUFFER_BUCKETS * sizeof(*fs->buffers));
        if (!fs->buffers) return NULL;
        memset(fs->buffers, 0, EXT4_BUFFER_BUCKETS * sizeof(*fs->buffers));
        fs->buffer_limit = EXT4_BUFFER_BYTES / fs->block_size;
    }
    struct ext4_buffer *buffer = (struct ext4_buffer *)kmalloc(sizeof(*buffer) + fs->block_size);
    if (!buffer) return NULL;
    buffer->block = block;
    buffer->dirty = 0;
    buffer->writing = 0;
    uint32_t bucket = buffer_bucket(block);
    buffer->hash_next = fs->buffers[bucket];
    fs->buffers[bucket] = buffer;
    lru_push(buffer);
    fs->buffer_count++;
    return buffer;
}

static void buffer_destroy(struct ext4_buffer *buffer) {
    struct ext4_buffer **link = &fs->buffers[buffer_bucket(buffer->block)];
    while (*link && *link != buffer) link = &(*link)->hash_next;
    if (*link) *link = buffer->hash_next;
    lru_unlink(buffer);
    if (buffer->dirty) fs->dirty_buffers--;
    fs->buffer_count--;
    kfree(buffer);
}

void buf_mark(uint8_t *data) {
    struct ext4_buffer *buffer = buffer_of(data);
    if (buffer->dirty) return;
    buffer->dirty = 1;
    fs->dirty_buffers++;
}

static int block_in_range(uint64_t block) { return block < fs->blocks_count; }

uint8_t *buf_get(uint64_t block) {
    if (!block_in_range(block)) return NULL;
    struct ext4_buffer *buffer = buffer_find(block);
    if (buffer) {
        lru_unlink(buffer);
        lru_push(buffer);
        return buffer->data;
    }
    buffer = buffer_create(block);
    if (!buffer) return NULL;
    if (ext4_volume_read(fs, block, 1, buffer->data) != 0) {
        buffer_destroy(buffer);
        return NULL;
    }
    return buffer->data;
}

uint8_t *buf_zero(uint64_t block) {
    if (!block_in_range(block)) return NULL;
    struct ext4_buffer *buffer = buffer_find(block);
    if (!buffer) buffer = buffer_create(block);
    if (!buffer) return NULL;
    memset(buffer->data, 0, fs->block_size);
    buf_mark(buffer->data);
    return buffer->data;
}

int buf_write(uint64_t block, const void *data) {
    if (!block_in_range(block)) return -1;
    struct ext4_buffer *buffer = buffer_find(block);
    if (!buffer) buffer = buffer_create(block);
    if (!buffer) return -1;
    memcpy(buffer->data, data, fs->block_size);
    buf_mark(buffer->data);
    return 0;
}

void buf_forget(uint64_t block) {
    struct ext4_buffer *buffer = buffer_find(block);
    if (buffer) buffer_destroy(buffer);
}

void buffers_trim(void) {
    struct ext4_buffer *buffer = fs->lru_tail;
    while (buffer && fs->buffer_count > fs->buffer_limit) {
        struct ext4_buffer *previous = buffer->lru_prev;
        if (!buffer->dirty && !buffer->writing) buffer_destroy(buffer);
        buffer = previous;
    }
}

void buffers_drop_all(void) {
    while (fs->lru_head) buffer_destroy(fs->lru_head);
    kfree(fs->buffers);
    fs->buffers = NULL;
    fs->buffer_count = 0;
    fs->dirty_buffers = 0;
}

int read_block(uint64_t block, void *out) {
    uint8_t *cached = buf_get(block);
    if (!cached) return -1;
    memcpy(out, cached, fs->block_size);
    return 0;
}

struct ext4_group_desc *gd(uint32_t group) {
    return (struct ext4_group_desc *)(fs->gdt + (size_t)group * fs->desc_size);
}

static int wide_desc(void) { return fs->desc_size >= EXT4_MIN_DESC_SIZE_64BIT; }

uint64_t gd_block_bitmap(uint32_t group) {
    struct ext4_group_desc *desc = gd(group);
    return desc->bg_block_bitmap_lo | (wide_desc() ? (uint64_t)desc->bg_block_bitmap_hi << 32 : 0);
}

uint64_t gd_inode_bitmap(uint32_t group) {
    struct ext4_group_desc *desc = gd(group);
    return desc->bg_inode_bitmap_lo | (wide_desc() ? (uint64_t)desc->bg_inode_bitmap_hi << 32 : 0);
}

uint64_t gd_inode_table(uint32_t group) {
    struct ext4_group_desc *desc = gd(group);
    return desc->bg_inode_table_lo | (wide_desc() ? (uint64_t)desc->bg_inode_table_hi << 32 : 0);
}

uint32_t gd_free_blocks(uint32_t group) {
    struct ext4_group_desc *desc = gd(group);
    return desc->bg_free_blocks_count_lo |
        (wide_desc() ? (uint32_t)desc->bg_free_blocks_count_hi << 16 : 0);
}

uint32_t gd_free_inodes(uint32_t group) {
    struct ext4_group_desc *desc = gd(group);
    return desc->bg_free_inodes_count_lo |
        (wide_desc() ? (uint32_t)desc->bg_free_inodes_count_hi << 16 : 0);
}

uint32_t gd_used_dirs(uint32_t group) {
    struct ext4_group_desc *desc = gd(group);
    return desc->bg_used_dirs_count_lo |
        (wide_desc() ? (uint32_t)desc->bg_used_dirs_count_hi << 16 : 0);
}

uint32_t gd_itable_unused(uint32_t group) {
    struct ext4_group_desc *desc = gd(group);
    return desc->bg_itable_unused_lo |
        (wide_desc() ? (uint32_t)desc->bg_itable_unused_hi << 16 : 0);
}

void gd_set_free_blocks(uint32_t group, uint32_t value) {
    struct ext4_group_desc *desc = gd(group);
    desc->bg_free_blocks_count_lo = (uint16_t)value;
    if (wide_desc()) desc->bg_free_blocks_count_hi = (uint16_t)(value >> 16);
    fs->group_dirty[group] |= 4U;
}

void gd_set_free_inodes(uint32_t group, uint32_t value) {
    struct ext4_group_desc *desc = gd(group);
    desc->bg_free_inodes_count_lo = (uint16_t)value;
    if (wide_desc()) desc->bg_free_inodes_count_hi = (uint16_t)(value >> 16);
    fs->group_dirty[group] |= 4U;
}

void gd_set_used_dirs(uint32_t group, uint32_t value) {
    struct ext4_group_desc *desc = gd(group);
    desc->bg_used_dirs_count_lo = (uint16_t)value;
    if (wide_desc()) desc->bg_used_dirs_count_hi = (uint16_t)(value >> 16);
    fs->group_dirty[group] |= 4U;
}

void gd_set_itable_unused(uint32_t group, uint32_t value) {
    struct ext4_group_desc *desc = gd(group);
    desc->bg_itable_unused_lo = (uint16_t)value;
    if (wide_desc()) desc->bg_itable_unused_hi = (uint16_t)(value >> 16);
    fs->group_dirty[group] |= 4U;
}

uint64_t sb_blocks_count(void) {
    return fs->sb.s_blocks_count_lo |
        (has_incompat(EXT4_INCOMPAT_64BIT) ? (uint64_t)fs->sb.s_blocks_count_hi << 32 : 0);
}

uint64_t sb_free_blocks(void) {
    return fs->sb.s_free_blocks_count_lo |
        (has_incompat(EXT4_INCOMPAT_64BIT) ? (uint64_t)fs->sb.s_free_blocks_count_hi << 32 : 0);
}

uint64_t sb_reserved_blocks(void) {
    return fs->sb.s_r_blocks_count_lo |
        (has_incompat(EXT4_INCOMPAT_64BIT) ? (uint64_t)fs->sb.s_r_blocks_count_hi << 32 : 0);
}

void sb_set_free_blocks(uint64_t value) {
    fs->sb.s_free_blocks_count_lo = (uint32_t)value;
    if (has_incompat(EXT4_INCOMPAT_64BIT)) fs->sb.s_free_blocks_count_hi = (uint32_t)(value >> 32);
}

uint64_t group_first_block(uint32_t group) {
    return fs->first_data_block + (uint64_t)group * fs->blocks_per_group;
}

uint32_t group_block_count(uint32_t group) {
    uint64_t first = group_first_block(group);
    if (first >= fs->blocks_count) return 0;
    uint64_t remaining = fs->blocks_count - first;
    return remaining < fs->blocks_per_group ? (uint32_t)remaining : fs->blocks_per_group;
}

static int power_of(uint32_t value, uint32_t base) {
    uint64_t power = base;
    while (power < value) power *= base;
    return power == value;
}

int group_has_super(uint32_t group) {
    if (group == 0) return 1;
    if (has_compat(EXT4_COMPAT_SPARSE_SUPER2))
        return group == fs->sb.s_backup_bgs[0] || group == fs->sb.s_backup_bgs[1];
    if (group <= 1 || !has_ro_compat(EXT4_RO_COMPAT_SPARSE_SUPER)) return 1;
    if (!(group & 1U)) return 0;
    return power_of(group, 3) || power_of(group, 5) || power_of(group, 7);
}

static uint32_t group_gdt_blocks_meta(uint32_t group) {
    uint32_t metagroup = group / fs->gd_per_block;
    uint32_t first = metagroup * fs->gd_per_block;
    uint32_t last = first + fs->gd_per_block - 1U;
    return group == first || group == first + 1U || group == last ? 1U : 0U;
}

uint32_t group_base_meta_blocks(uint32_t group) {
    uint32_t count = (uint32_t)group_has_super(group);
    if (!has_incompat(EXT4_INCOMPAT_META_BG) || group < fs->sb.s_first_meta_bg * fs->gd_per_block) {
        if (count) {
            count += has_incompat(EXT4_INCOMPAT_META_BG) ? fs->sb.s_first_meta_bg : fs->gd_blocks;
            count += fs->sb.s_reserved_gdt_blocks;
        }
    } else {
        count += group_gdt_blocks_meta(group);
    }
    return count;
}

uint64_t gd_location(uint32_t index) {
    if (!has_incompat(EXT4_INCOMPAT_META_BG) || index < fs->sb.s_first_meta_bg)
        return fs->sb_home_block + 1U + index;
    uint32_t group = index * fs->gd_per_block;
    uint64_t skip = (uint64_t)group_has_super(group);
    if (fs->block_size == 1024U && index == 0 && fs->first_data_block == 0) skip++;
    return group_first_block(group) + skip;
}

static uint16_t group_desc_csum(uint32_t group) {
    uint8_t *desc = (uint8_t *)gd(group);
    uint32_t offset = offsetof(struct ext4_group_desc, bg_checksum);
    uint32_t le_group = group;
    if (has_csum()) {
        uint16_t dummy = 0;
        uint32_t sum = crc32c(fs->csum_seed, &le_group, sizeof(le_group));
        sum = crc32c(sum, desc, offset);
        sum = crc32c(sum, &dummy, sizeof(dummy));
        if (offset + 2U < fs->desc_size)
            sum = crc32c(sum, desc + offset + 2U, fs->desc_size - offset - 2U);
        return (uint16_t)sum;
    }
    if (!has_ro_compat(EXT4_RO_COMPAT_GDT_CSUM)) return 0;
    uint16_t crc = crc16(0xFFFFU, fs->sb.s_uuid, sizeof(fs->sb.s_uuid));
    crc = crc16(crc, &le_group, sizeof(le_group));
    crc = crc16(crc, desc, offset);
    if (has_incompat(EXT4_INCOMPAT_64BIT) && offset + 2U < fs->desc_size)
        crc = crc16(crc, desc + offset + 2U, fs->desc_size - offset - 2U);
    return crc;
}

static int group_desc_valid(uint32_t group) {
    if (!has_group_csum()) return 1;
    return gd(group)->bg_checksum == group_desc_csum(group);
}

void group_seal(uint32_t group) {
    struct ext4_group_desc *desc = gd(group);
    uint8_t dirty = fs->group_dirty[group];
    if (has_csum() && (dirty & 1U) && !(desc->bg_flags & EXT4_BG_BLOCK_UNINIT)) {
        uint8_t *bits = buf_get(gd_block_bitmap(group));
        if (bits) {
            uint32_t sum = crc32c(fs->csum_seed, bits, fs->blocks_per_group / 8U);
            desc->bg_block_bitmap_csum_lo = (uint16_t)sum;
            if (fs->desc_size >= offsetof(struct ext4_group_desc, bg_block_bitmap_csum_hi) + 2U)
                desc->bg_block_bitmap_csum_hi = (uint16_t)(sum >> 16);
        }
    }
    if (has_csum() && (dirty & 2U) && !(desc->bg_flags & EXT4_BG_INODE_UNINIT)) {
        uint8_t *bits = buf_get(gd_inode_bitmap(group));
        if (bits) {
            uint32_t sum = crc32c(fs->csum_seed, bits, fs->inodes_per_group / 8U);
            desc->bg_inode_bitmap_csum_lo = (uint16_t)sum;
            if (fs->desc_size >= offsetof(struct ext4_group_desc, bg_inode_bitmap_csum_hi) + 2U)
                desc->bg_inode_bitmap_csum_hi = (uint16_t)(sum >> 16);
        }
    }
    if (dirty) desc->bg_checksum = group_desc_csum(group);
    fs->group_dirty[group] = 0;
}

void groups_seal_all(void) {
    for (uint32_t group = 0; group < fs->group_count; group++)
        if (fs->group_dirty[group]) group_seal(group);
}

static int stage_groups(void) {
    groups_seal_all();
    for (uint32_t index = 0; index < fs->gd_blocks; index++) {
        uint32_t start = index * fs->gd_per_block;
        uint32_t count = fs->group_count - start;
        if (count > fs->gd_per_block) count = fs->gd_per_block;
        size_t offset = (size_t)start * fs->desc_size;
        size_t bytes = (size_t)count * fs->desc_size;
        if (memcmp(fs->gdt + offset, fs->gdt_written + offset, bytes) == 0) continue;
        uint8_t *block = buf_get(gd_location(index));
        if (!block) return -1;
        memcpy(block, fs->gdt + offset, bytes);
        buf_mark(block);
        memcpy(fs->gdt_written + offset, fs->gdt + offset, bytes);
    }
    return 0;
}

static int stage_superblock(void) {
    struct ext4_super_block *sb = &fs->sb;
    sb->s_checksum = fs->sb_written.s_checksum;
    sb->s_wtime = fs->sb_written.s_wtime;
    if (memcmp(sb, &fs->sb_written, sizeof(*sb)) == 0) return 0;
    sb->s_wtime = ext4_now();
    ext4_superblock_csum_set(sb);
    uint8_t *home = buf_get(fs->sb_home_block);
    if (!home) return -1;
    memcpy(home + fs->sb_home_offset, sb, sizeof(*sb));
    buf_mark(home);
    fs->sb_written = *sb;
    return 0;
}

static int write_superblock_direct(void) {
    fs->sb.s_wtime = ext4_now();
    ext4_superblock_csum_set(&fs->sb);
    uint8_t *home = buf_get(fs->sb_home_block);
    if (!home) return -1;
    memcpy(home + fs->sb_home_offset, &fs->sb, sizeof(fs->sb));
    fs->sb_written = fs->sb;
    if (ext4_volume_write(fs, fs->sb_home_block, 1, home) != 0) return -1;
    struct ext4_buffer *buffer = buffer_of(home);
    if (buffer->dirty) {
        buffer->dirty = 0;
        fs->dirty_buffers--;
    }
    return 0;
}

static void sort_blocks(uint64_t *targets, uint8_t **data, uint32_t count) {
    for (uint32_t gap = count / 2U; gap; gap /= 2U) {
        for (uint32_t index = gap; index < count; index++) {
            uint64_t target = targets[index];
            uint8_t *bytes = data[index];
            uint32_t at = index;
            while (at >= gap && targets[at - gap] > target) {
                targets[at] = targets[at - gap];
                data[at] = data[at - gap];
                at -= gap;
            }
            targets[at] = target;
            data[at] = bytes;
        }
    }
}

struct commit_plan {
    uint32_t count;
    uint64_t *targets;
    uint8_t **data;
    uint8_t *snapshot;
};

static int commit_prepare(struct commit_plan *plan) {
    memset(plan, 0, sizeof(*plan));
    apply_pending_frees();
    if (stage_groups() != 0 || stage_superblock() != 0) return -1;
    fs->last_commit_ns = time_uptime_ns();
    uint32_t count = fs->dirty_buffers;
    if (!count) return 0;
    plan->targets = (uint64_t *)kmalloc(count * sizeof(*plan->targets));
    plan->data = (uint8_t **)kmalloc(count * sizeof(*plan->data));
    plan->snapshot = (uint8_t *)kmalloc((size_t)count * fs->block_size);
    if (!plan->targets || !plan->data || !plan->snapshot) return -1;
    uint32_t found = 0;
    for (struct ext4_buffer *buffer = fs->lru_head; buffer && found < count;
         buffer = buffer->lru_next) {
        if (!buffer->dirty) continue;
        plan->targets[found] = buffer->block;
        plan->data[found] = buffer->data;
        found++;
    }
    sort_blocks(plan->targets, plan->data, found);
    for (uint32_t index = 0; index < found; index++) {
        struct ext4_buffer *buffer = buffer_of(plan->data[index]);
        uint8_t *copy = plan->snapshot + (size_t)index * fs->block_size;
        memcpy(copy, buffer->data, fs->block_size);
        plan->data[index] = copy;
        buffer->dirty = 0;
        buffer->writing = 1;
        fs->dirty_buffers--;
    }
    plan->count = found;
    return 0;
}

static int commit_write(struct ext4_volume *volume, const struct commit_plan *plan) {
    if (!plan->count) return 0;
    if (volume->journal)
        return jbd2_journal_commit(volume->journal, plan->count, plan->targets, plan->data);
    uint32_t per_run = EXT4_RUN_BYTES / volume->block_size;
    for (uint32_t index = 0; index < plan->count;) {
        uint32_t run = 1;
        while (index + run < plan->count && run < per_run &&
               plan->targets[index + run] == plan->targets[index] + run)
            run++;
        if (run == 1) {
            if (ext4_volume_write(volume, plan->targets[index], 1, plan->data[index]) != 0)
                return -1;
        } else {
            uint8_t *gather = (uint8_t *)kmalloc((size_t)run * volume->block_size);
            if (!gather) return -1;
            for (uint32_t part = 0; part < run; part++)
                memcpy(gather + (size_t)part * volume->block_size, plan->data[index + part],
                       volume->block_size);
            int status = ext4_volume_write(volume, plan->targets[index], run, gather);
            kfree(gather);
            if (status != 0) return -1;
        }
        index += run;
    }
    return 0;
}

static void commit_finish(struct commit_plan *plan, int status) {
    for (uint32_t index = 0; index < plan->count; index++) {
        struct ext4_buffer *buffer = buffer_find(plan->targets[index]);
        if (!buffer) continue;
        buffer->writing = 0;
        if (status != 0) buf_mark(buffer->data);
    }
    kfree(plan->targets);
    kfree(plan->data);
    kfree(plan->snapshot);
    buffers_trim();
}

int ext4_volume_commit(void) {
    struct commit_plan plan;
    int status = commit_prepare(&plan);
    if (status == 0) status = commit_write(fs, &plan);
    commit_finish(&plan, status);
    return status;
}

static int commit_volume(struct ext4_volume *volume) {
    struct ext4_volume *saved = ext4_enter(volume);
    struct commit_plan plan;
    int status = commit_prepare(&plan);
    fs = saved;
    mutex_unlock(&ext4_lock);
    if (status == 0) status = commit_write(volume, &plan);
    mutex_lock(&ext4_lock);
    saved = ext4_enter(volume);
    commit_finish(&plan, status);
    if (status != 0) {
        ext4_failed_stage = "journal";
        ext4_report_failure("commit", "metadata");
    }
    fs = saved;
    return status;
}

static uint32_t commit_threshold(void) {
    uint32_t limit = fs->buffer_limit ? fs->buffer_limit / 2U : 1024U;
    if (fs->journal) {
        uint32_t capacity = jbd2_journal_capacity(fs->journal) / 2U;
        if (capacity && capacity < limit) limit = capacity;
    }
    return limit;
}

int ext4_flush_meta(void) {
    uint32_t threshold = commit_threshold();
    uint32_t load = fs->dirty_buffers + fs->pending_free_count / 64U;
    if (load >= threshold) ext4_commit_kick();
    if (load >= 2U * threshold) __atomic_store_n(&commit_behind, 1, __ATOMIC_RELEASE);
    return 0;
}

static int groups_changed(void) {
    for (uint32_t group = 0; group < fs->group_count; group++)
        if (fs->group_dirty[group]) return 1;
    return memcmp(fs->gdt, fs->gdt_written, (size_t)fs->group_count * fs->desc_size) != 0;
}

static int volume_needs_commit(void) {
    if (fs->dirty_buffers || fs->pending_free_count || groups_changed()) return 1;
    struct ext4_super_block copy = fs->sb;
    copy.s_checksum = fs->sb_written.s_checksum;
    copy.s_wtime = fs->sb_written.s_wtime;
    return memcmp(&copy, &fs->sb_written, sizeof(copy)) != 0;
}

int ext4_commit_volumes(int force, int flush) {
    int status = 0;
    mutex_lock(&ext4_io_lock);
    mutex_lock(&ext4_lock);
    uint64_t now = time_uptime_ns();
    int behind = 0;
    for (struct ext4_volume *volume = ext4_volumes; volume; volume = volume->next) {
        if (volume->loading || volume->read_only) continue;
        struct ext4_volume *saved = ext4_enter(volume);
        mmp_tick();
        int due = volume_needs_commit() &&
            (force || now - fs->last_commit_ns >= EXT4_COMMIT_INTERVAL_NS ||
             fs->dirty_buffers + fs->pending_free_count / 64U >= commit_threshold());
        fs = saved;
        if (due && commit_volume(volume) != 0) status = -1;
        saved = ext4_enter(volume);
        if (fs->buffer_count > fs->buffer_limit) buffers_trim();
        if (fs->dirty_buffers + fs->pending_free_count / 64U >= 2U * commit_threshold()) behind = 1;
        fs = saved;
    }
    __atomic_store_n(&commit_behind, behind, __ATOMIC_RELEASE);
    mutex_unlock(&ext4_lock);
    if (flush) {
        for (struct ext4_volume *volume = ext4_volumes; volume; volume = volume->next)
            if (!volume->loading && !volume->read_only && ext4_volume_flush(volume) != 0)
                status = -1;
    }
    mutex_unlock(&ext4_io_lock);
    return status;
}

int ext4_sync_volume(struct ext4_volume *volume) {
    mutex_lock(&ext4_io_lock);
    mutex_lock(&ext4_lock);
    int status = volume && !volume->read_only ? commit_volume(volume) : 0;
    mutex_unlock(&ext4_lock);
    if (volume && !volume->read_only && ext4_volume_flush(volume) != 0) status = -1;
    mutex_unlock(&ext4_io_lock);
    return status;
}

void ext4fs_balance(void) {
    if (!__atomic_load_n(&commit_behind, __ATOMIC_ACQUIRE) || !process_may_sleep()) return;
    (void)ext4_commit_volumes(1, 0);
}

static int journal_read_block(void *context, uint64_t block, void *out) {
    return ext4_volume_read((struct ext4_volume *)context, block, 1, out);
}

static int journal_write_block(void *context, uint64_t block, const void *data) {
    return ext4_volume_write((struct ext4_volume *)context, block, 1, data);
}

static int journal_write_run(void *context, uint64_t block, uint32_t count, const void *data) {
    return ext4_volume_write((struct ext4_volume *)context, block, count, data);
}

static int journal_flush_device(void *context) {
    return ext4_volume_flush((struct ext4_volume *)context);
}

static int journal_map_block(void *context, uint32_t file_block, uint64_t *disk_block) {
    struct ext4_volume *volume = (struct ext4_volume *)context;
    struct ext4_volume *saved = ext4_enter(volume);
    uint32_t run = 0;
    int unwritten = 0;
    int64_t block = inode_map(fs->journal_ino, &fs->journal_inode, file_block, &run, &unwritten);
    fs = saved;
    if (block <= 0) return -1;
    *disk_block = (uint64_t)block;
    return 0;
}

static const struct jbd2_journal_ops journal_ops = {
    .read = journal_read_block,
    .write = journal_write_block,
    .map = journal_map_block,
    .flush = journal_flush_device,
    .write_run = journal_write_run,
};

static int journal_start_up(void) {
    jbd2_journal_close(fs->journal);
    fs->journal = NULL;
    if (!has_compat(EXT4_COMPAT_HAS_JOURNAL)) return has_incompat(EXT4_INCOMPAT_RECOVER) ? -1 : 0;
    if (!fs->sb.s_journal_inum) {
        if (has_incompat(EXT4_INCOMPAT_RECOVER) || !fs->read_only) {
            kprintf("EXT4: %s has its journal on another device\n", fs->device->dev_name);
            return -1;
        }
        return 0;
    }
    fs->journal_ino = fs->sb.s_journal_inum;
    if (inode_read(fs->journal_ino, &fs->journal_inode) != 0) return -1;
    fs->journal = jbd2_journal_open(&journal_ops, fs, fs->block_size);
    if (!fs->journal) return -1;
    return jbd2_journal_recover(fs->journal);
}

static int superblock_usable(void) {
    const struct ext4_super_block *sb = &fs->sb;
    if (sb->s_magic != EXT4_SUPER_MAGIC) return 0;
    if (sb->s_rev_level > 1) {
        kprintf("EXT4: %s has revision %u\n", fs->device->dev_name, (unsigned)sb->s_rev_level);
        return 0;
    }
    if (sb->s_log_block_size > 2) {
        kprintf("EXT4: %s uses %u KiB blocks, larger than a page\n", fs->device->dev_name,
                (unsigned)(1U << sb->s_log_block_size));
        return 0;
    }
    uint32_t block_size = 1024U << sb->s_log_block_size;
    uint32_t inode_size = sb->s_rev_level ? sb->s_inode_size : EXT4_GOOD_OLD_INODE_SIZE;
    uint64_t blocks = sb->s_blocks_count_lo;
    if (sb->s_rev_level && (sb->s_feature_incompat & EXT4_INCOMPAT_64BIT))
        blocks |= (uint64_t)sb->s_blocks_count_hi << 32;
    uint64_t usable_blocks = fs->device->sectors / (block_size / BLOCK_SECTOR_SIZE);
    int bigalloc = sb->s_rev_level && (sb->s_feature_ro_compat & EXT4_RO_COMPAT_BIGALLOC);
    uint32_t per_group_bits = bigalloc ? sb->s_clusters_per_group : sb->s_blocks_per_group;
    if (inode_size < EXT4_GOOD_OLD_INODE_SIZE || inode_size > block_size ||
        (inode_size & (inode_size - 1U)) ||
        (!bigalloc && sb->s_first_data_block != (block_size == 1024U ? 1U : 0U)) ||
        !sb->s_blocks_per_group || !sb->s_inodes_per_group || per_group_bits > 8U * block_size ||
        sb->s_inodes_per_group > 8U * block_size ||
        sb->s_inodes_per_group % (block_size / inode_size) || blocks > usable_blocks ||
        blocks <= sb->s_first_data_block)
        return 0;
    if (sb->s_rev_level) {
        uint32_t incompat = sb->s_feature_incompat;
        uint32_t unknown = incompat & ~(EXT4_INCOMPAT_SUPPORTED | EXT4_INCOMPAT_READ_ONLY);
        if (unknown) {
            kprintf("EXT4: %s needs features %x this kernel does not have\n", fs->device->dev_name,
                    (unsigned)unknown);
            return 0;
        }
        if (incompat & EXT4_INCOMPAT_64BIT) {
            uint32_t desc = sb->s_desc_size;
            if (desc < EXT4_MIN_DESC_SIZE_64BIT || desc > block_size || (desc & (desc - 1U)))
                return 0;
        }
    }

    uint64_t groups =
        (blocks - sb->s_first_data_block + sb->s_blocks_per_group - 1U) / sb->s_blocks_per_group;
    if (!groups || groups > 0xFFFFFFFFULL) return 0;
    if ((uint64_t)sb->s_inodes_count != groups * sb->s_inodes_per_group) return 0;
    if (!sb->s_rev_level) return 1;
    if ((sb->s_feature_incompat & EXT4_INCOMPAT_READ_ONLY) ||
        (sb->s_feature_ro_compat & ~EXT4_RO_COMPAT_WRITABLE))
        return 2;
    return 1;
}

static void adopt_geometry(void) {
    const struct ext4_super_block *sb = &fs->sb;
    fs->block_size = 1024U << sb->s_log_block_size;
    fs->sectors_per_block = fs->block_size / BLOCK_SECTOR_SIZE;
    fs->inode_size = sb->s_rev_level ? sb->s_inode_size : EXT4_GOOD_OLD_INODE_SIZE;
    fs->inodes_per_block = fs->block_size / fs->inode_size;
    fs->pointers = fs->block_size / 4U;
    fs->desc_size = has_incompat(EXT4_INCOMPAT_64BIT) ? sb->s_desc_size : EXT4_MIN_DESC_SIZE;
    fs->gd_per_block = fs->block_size / fs->desc_size;
    fs->sb_home_block = 1024U / fs->block_size;
    fs->sb_home_offset = 1024U % fs->block_size;
    fs->first_data_block = sb->s_first_data_block;
    fs->blocks_per_group = sb->s_blocks_per_group;
    fs->inodes_per_group = sb->s_inodes_per_group;
    fs->blocks_count = sb_blocks_count();
    fs->group_count =
        (uint32_t)((fs->blocks_count - fs->first_data_block + fs->blocks_per_group - 1U) /
                   fs->blocks_per_group);
    fs->gd_blocks = (fs->group_count + fs->gd_per_block - 1U) / fs->gd_per_block;

    fs->extra_isize = 0;
    uint32_t room = fs->inode_size - EXT4_GOOD_OLD_INODE_SIZE;
    if (room >= 4U) {
        uint32_t extra = has_ro_compat(EXT4_RO_COMPAT_EXTRA_ISIZE) ? sb->s_want_extra_isize : 0;
        uint32_t least = has_ro_compat(EXT4_RO_COMPAT_EXTRA_ISIZE) ? sb->s_min_extra_isize : 0;
        if (extra < least) extra = least;
        if (extra < EXT4_DEFAULT_EXTRA_ISIZE) extra = EXT4_DEFAULT_EXTRA_ISIZE;
        if (extra > room) extra = room;
        fs->extra_isize = (uint16_t)(extra & ~3U);
    }

    if (has_incompat(EXT4_INCOMPAT_CSUM_SEED)) fs->csum_seed = sb->s_checksum_seed;
    else fs->csum_seed = crc32c(0xFFFFFFFFU, sb->s_uuid, sizeof(sb->s_uuid));
    memcpy(fs->hash_seed, sb->s_hash_seed, sizeof(fs->hash_seed));
    fs->hash_unsigned = (sb->s_flags & EXT4_FLAGS_UNSIGNED_HASH) ? 3U : 0U;
}

static int read_metadata(void) {
    if (block_device_read(fs->device, 1024U / BLOCK_SECTOR_SIZE, 2, meta_buf) != 0) return -EIO;
    memcpy(&fs->sb, meta_buf, sizeof(fs->sb));
    int usable = superblock_usable();
    if (!usable) return -EINVAL;
    if (usable == 2 && !fs->read_only) {
        kprintf("EXT4: %s has features only safe to read, mounting it read-only\n",
                fs->device->dev_name);
        fs->read_only = 1;
    }
    if (has_csum()) {
        if (fs->sb.s_checksum_type != 1U) return -EINVAL;
        if (crc32c(0xFFFFFFFFU, &fs->sb, offsetof(struct ext4_super_block, s_checksum)) !=
            fs->sb.s_checksum) {
            kprintf("EXT4: %s has a bad superblock checksum\n", fs->device->dev_name);
            return -EINVAL;
        }
    }
    adopt_geometry();

    kfree(fs->gdt);
    kfree(fs->gdt_written);
    kfree(fs->group_dirty);
    size_t table_bytes = (size_t)fs->group_count * fs->desc_size;
    fs->gdt = (uint8_t *)kmalloc(table_bytes);
    fs->gdt_written = (uint8_t *)kmalloc(table_bytes);
    fs->group_dirty = (uint8_t *)kmalloc(fs->group_count);
    if (!fs->gdt || !fs->gdt_written || !fs->group_dirty) return -ENOMEM;
    memset(fs->group_dirty, 0, fs->group_count);

    for (uint32_t index = 0; index < fs->gd_blocks; index++) {
        if (ext4_volume_read(fs, gd_location(index), 1, meta_buf) != 0) return -EIO;
        uint32_t start = index * fs->gd_per_block;
        uint32_t count = fs->group_count - start;
        if (count > fs->gd_per_block) count = fs->gd_per_block;
        memcpy(fs->gdt + (size_t)start * fs->desc_size, meta_buf, (size_t)count * fs->desc_size);
    }
    uint32_t table_blocks = fs->inodes_per_group / fs->inodes_per_block;
    for (uint32_t group = 0; group < fs->group_count; group++) {
        if (gd_block_bitmap(group) >= fs->blocks_count ||
            gd_inode_bitmap(group) >= fs->blocks_count ||
            gd_inode_table(group) + table_blocks > fs->blocks_count) {
            kprintf("EXT4: %s group %u points outside the filesystem\n", fs->device->dev_name,
                    (unsigned)group);
            return -EINVAL;
        }
        if (!group_desc_valid(group)) {
            kprintf("EXT4: %s group %u has a bad descriptor checksum\n", fs->device->dev_name,
                    (unsigned)group);
            return -EINVAL;
        }
    }
    memcpy(fs->gdt_written, fs->gdt, table_bytes);
    fs->sb_written = fs->sb;
    return 0;
}

static uint32_t journal_wanted(void) {
    uint32_t wanted = 0;
    if (has_incompat(EXT4_INCOMPAT_64BIT)) wanted |= JBD2_WANT_64BIT;
    if (has_csum()) wanted |= JBD2_WANT_CSUM;
    return wanted;
}

static void settle_hash_flags(void) {
    if (fs->sb.s_flags & (EXT4_FLAGS_SIGNED_HASH | EXT4_FLAGS_UNSIGNED_HASH)) return;
#if defined(__CHAR_UNSIGNED__)
    fs->sb.s_flags |= EXT4_FLAGS_UNSIGNED_HASH;
    fs->hash_unsigned = 3U;
#else
    fs->sb.s_flags |= EXT4_FLAGS_SIGNED_HASH;
#endif
}

static int volume_open(void) {
    buffers_drop_all();
    int status = read_metadata();
    if (status != 0) return status;
    if (journal_start_up() != 0) return -EIO;
    buffers_drop_all();
    status = read_metadata();
    if (status != 0) return status;
    if (!(fs->sb.s_state & EXT4_VALID_FS))
        kprintf("EXT4: %s was not cleanly unmounted, running e2fsck is recommended\n",
                fs->device->dev_name);
    if (fs->sb.s_state & EXT4_ERROR_FS)
        kprintf("EXT4: %s has errors recorded, running e2fsck is recommended\n",
                fs->device->dev_name);
    if (fs->read_only) {
        fs->last_commit_ns = time_uptime_ns();
        return 0;
    }
    if (mmp_start() != 0) return -EBUSY;
    if (orphans_open() != 0) return -EIO;
    settle_hash_flags();
    fs->sb.s_mnt_count++;
    fs->sb.s_mtime = ext4_now();
    if (fs->journal) {
        fs->sb.s_feature_incompat |= EXT4_INCOMPAT_RECOVER;
        if (write_superblock_direct() != 0 || ext4_volume_flush(fs) != 0) return -EIO;
        if (jbd2_journal_begin(fs->journal, journal_wanted()) != 0) return -EIO;
    } else {
        fs->sb.s_feature_incompat &= ~EXT4_INCOMPAT_RECOVER;
        fs->sb.s_state &= (uint16_t)~EXT4_VALID_FS;
        if (write_superblock_direct() != 0 || ext4_volume_flush(fs) != 0) return -EIO;
    }
    if (orphans_replay() != 0) return -EIO;
    fs->last_commit_ns = time_uptime_ns();
    return 0;
}

static int volume_shutdown(void) {
    if (fs->read_only) return 0;
    orphans_settle();
    int status = ext4_volume_commit();
    if (fs->journal && jbd2_journal_end(fs->journal) != 0) status = -1;
    fs->sb.s_feature_incompat &= ~EXT4_INCOMPAT_RECOVER;
    if (status == 0) fs->sb.s_state |= EXT4_VALID_FS;
    if (write_superblock_direct() != 0) status = -1;
    if (ext4_volume_flush(fs) != 0) status = -1;
    mmp_stop();
    return status;
}

static void volume_destroy(struct ext4_volume *volume) {
    struct ext4_volume *saved = ext4_enter(volume);
    buffers_drop_all();
    fs = saved;
    jbd2_journal_close(volume->journal);
    kfree(volume->gdt);
    kfree(volume->gdt_written);
    kfree(volume->group_dirty);
    kfree(volume->pending_free);
    kfree(volume);
}

static void volume_unlist(struct ext4_volume *volume) {
    for (struct ext4_volume **link = &ext4_volumes; *link; link = &(*link)->next) {
        if (*link != volume) continue;
        *link = volume->next;
        return;
    }
}

static int mount_volume(const struct block_device *device, struct vfs_node *root, int read_only,
                        struct ext4_volume **out) {
    struct ext4_volume *volume = (struct ext4_volume *)kmalloc(sizeof(*volume));
    if (!volume) return -ENOMEM;
    memset(volume, 0, sizeof(*volume));
    volume->device = device;
    volume->root = root;
    volume->loading = 1;
    volume->read_only = read_only || !device->write;

    mutex_lock(&ext4_io_lock);
    struct ext4_volume *saved = ext4_op_enter(volume);
    int status = volume_open();
    if (status == 0) {
        volume->next = ext4_volumes;
        ext4_volumes = volume;
        root->disk_inode = EXT4_ROOT_INO;
        root->fs_private = volume;
        struct ext4_inode inode;
        if (inode_read(EXT4_ROOT_INO, &inode) == 0) ext4_restore_owner(root, &inode);
        if (volume->read_only) root->flags |= VFS_READONLY;
        int restored = ext4_load_tree(EXT4_ROOT_INO, root);
        ext4_links_reset();
        if (restored < 0) {
            kprintf("EXT4: %s could not be read\n", device->dev_name);
            volume_unlist(volume);
            root->disk_inode = 0;
            root->fs_private = NULL;
            status = -EIO;
        } else {
            KDEBUG("EXT4: %s loaded, %d entries (%u/%u blocks free)\n", device->dev_name, restored,
                   (unsigned)sb_free_blocks(), (unsigned)fs->blocks_count);
        }
    }
    if (status != 0) volume_destroy(volume);
    ext4_op_leave(saved);
    mutex_unlock(&ext4_io_lock);
    if (status != 0) return status;
    ext4_set_persist_ops();
    *out = volume;
    return 0;
}

static const struct {
    const char *path;
    uint32_t mode;
} ext4_volatile_dirs[] = {
    {"/tmp", 01777}, {"/var/tmp", 01777}, {"/run", 0755},
    {"/dev", 0755},  {"/proc", 0555},     {"/sys", 0555},
};

static void mark_volatile_dirs(void) {
    for (size_t index = 0; index < sizeof(ext4_volatile_dirs) / sizeof(ext4_volatile_dirs[0]);
         index++) {
        struct vfs_node *node = vfs_lookup(ext4_volatile_dirs[index].path);
        if (!node) {
            node = vfs_mkdir_p(ext4_volatile_dirs[index].path);
            if (!node) continue;
            node->mode = ext4_volatile_dirs[index].mode;
        }
        node->flags |= VFS_VOLATILE;
        if (strcmp(ext4_volatile_dirs[index].path, "/dev") != 0 &&
            strcmp(ext4_volatile_dirs[index].path, "/proc") != 0 &&
            strcmp(ext4_volatile_dirs[index].path, "/sys") != 0)
            vfs_mount_builtin("tmpfs", ext4_volatile_dirs[index].path, "tmpfs", node);
    }
}

int ext4fs_find_label(const char *label) {
    VFS_GUARD;
    if (!label || !*label) return -1;
    int count = block_device_count();
    for (int index = 0; index < count; index++) {
        const struct block_device *device = block_device_at(index);
        if (!device || !device->read) continue;
        struct ext4_super_block probe;
        if (block_device_read_bytes(device, 1024, sizeof probe, &probe) != 0) continue;
        if (probe.s_magic != EXT4_SUPER_MAGIC) continue;
        size_t length = 0;
        while (length < sizeof probe.s_volume_name && probe.s_volume_name[length]) length++;
        if (strlen(label) != length) continue;
        if (strncmp(probe.s_volume_name, label, length) == 0) return index;
    }
    return -1;
}

int ext4fs_mount_root(int read_only) {
    VFS_GUARD;
    const struct block_device *device = block_root();
    if (!vfs_root || !device || ext4_volume_of(vfs_root)) return -1;
    struct ext4_volume *volume;
    if (mount_volume(device, vfs_root, read_only, &volume) != 0) return -1;
    if (!volume->read_only) mark_volatile_dirs();
    volume->loading = 0;
    return 0;
}

static int devices_overlap(const struct block_device *a, const struct block_device *b) {
    if (a == b) return 1;
    if (a->parent >= 0 && block_device_at(a->parent) == b) return 1;
    return b->parent >= 0 && block_device_at(b->parent) == a;
}

int ext4fs_mount(const char *source, const char *mount_name, int read_only,
                 struct vfs_node **root_out) {
    VFS_GUARD;
    *root_out = NULL;
    if (!source) return -EINVAL;
    int index = block_device_index_by_name(source);
    const struct block_device *device = index < 0 ? NULL : block_device_at(index);
    if (!device || !device->read) return -ENODEV;
    for (struct ext4_volume *volume = ext4_volumes; volume; volume = volume->next)
        if (devices_overlap(volume->device, device)) return -EBUSY;

    struct vfs_node *root = vfs_alloc_node(mount_name, VFS_DIRECTORY);
    if (!root) return -ENOMEM;
    *root_out = root;
    struct ext4_volume *volume;
    int status = mount_volume(device, root, read_only, &volume);
    if (status != 0) return status;
    volume->loading = 0;
    return 0;
}

void ext4fs_unmount(struct vfs_node *root) {
    VFS_GUARD;
    mutex_lock(&ext4_io_lock);
    mutex_lock(&ext4_lock);
    struct ext4_volume *volume = ext4_volumes;
    while (volume && volume->root != root) volume = volume->next;
    if (volume) {
        struct ext4_volume *saved = ext4_enter(volume);
        if (volume_shutdown() != 0)
            kprintf("EXT4: %s did not unmount cleanly\n", volume->device->dev_name);
        fs = saved;
        volume_unlist(volume);
        volume_destroy(volume);
    }
    mutex_unlock(&ext4_lock);
    mutex_unlock(&ext4_io_lock);
}

int ext4fs_owns(const struct vfs_node *node) {
    VFS_GUARD;
    EXT4_LOCKED;
    return ext4_volume_of(node) != NULL;
}

const char *ext4fs_type_name(const struct vfs_node *node) {
    VFS_GUARD;
    EXT4_LOCKED;
    struct ext4_volume *volume = ext4_volume_of(node);
    if (!volume) return "ext2";
    const struct ext4_super_block *sb = &volume->sb;
    if ((sb->s_feature_incompat &
         ~(EXT4_INCOMPAT_FILETYPE | EXT4_INCOMPAT_RECOVER | EXT4_INCOMPAT_META_BG)) ||
        (sb->s_feature_ro_compat &
         ~(EXT4_RO_COMPAT_SPARSE_SUPER | EXT4_RO_COMPAT_LARGE_FILE | EXT4_RO_COMPAT_BTREE_DIR)))
        return "ext4";
    return (sb->s_feature_compat & EXT4_COMPAT_HAS_JOURNAL) ? "ext3" : "ext2";
}

int ext4fs_read_only(const struct vfs_node *node) {
    VFS_GUARD;
    EXT4_LOCKED;
    struct ext4_volume *volume = ext4_volume_of(node);
    return volume && volume->read_only;
}

int ext4fs_stats(const struct vfs_node *node, struct ext4_fs_stats *out) {
    VFS_GUARD;
    EXT4_LOCKED;
    struct ext4_volume *volume = NULL;
    for (const struct vfs_node *walk = node; walk && !volume; walk = walk->parent) {
        volume = ext4_volume_of(walk);
        if (walk->parent == walk) break;
    }
    if (!volume || !out) return -1;
    struct ext4_volume *saved = ext4_enter(volume);
    out->block_size = volume->block_size;
    out->blocks = volume->blocks_count;
    out->free_blocks = sb_free_blocks();
    out->reserved_blocks = sb_reserved_blocks();
    out->inodes = volume->sb.s_inodes_count;
    out->free_inodes = volume->sb.s_free_inodes_count;
    out->name_max = EXT4_NAME_LEN;
    out->read_only = volume->read_only;
    memcpy(out->uuid, volume->sb.s_uuid, sizeof(out->uuid));
    fs = saved;
    return 0;
}

int ext4fs_shutdown(void) {
    VFS_GUARD;
    mutex_lock(&ext4_io_lock);
    mutex_lock(&ext4_lock);
    int status = 0;
    for (struct ext4_volume *volume = ext4_volumes; volume; volume = volume->next) {
        struct ext4_volume *saved = ext4_enter(volume);
        if (volume_shutdown() != 0) status = -1;
        fs = saved;
    }
    mutex_unlock(&ext4_lock);
    mutex_unlock(&ext4_io_lock);
    return status;
}

static const char commit_channel;
static volatile int commit_requested;

void ext4_commit_kick(void) {
    if (__atomic_exchange_n(&commit_requested, 1, __ATOMIC_ACQ_REL)) return;
    process_wake_all(&commit_channel);
}

static void commit_thread(void *unused) {
    (void)unused;
    for (;;) {
        process_prepare_wait(&commit_channel, time_uptime_ns() + EXT4_COMMIT_POLL_NS);
        if (!__atomic_load_n(&commit_requested, __ATOMIC_ACQUIRE)) process_wait();
        process_finish_wait();
        int urgent = __atomic_exchange_n(&commit_requested, 0, __ATOMIC_ACQ_REL);
        (void)ext4_commit_volumes(urgent, 0);
    }
}

void ext4fs_start(void) {
    if (!process_create_kthread("ext4commit", commit_thread, NULL))
        kprintf("EXT4: cannot start the commit thread\n");
}
