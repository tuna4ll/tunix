#include <stddef.h>
#include <stdint.h>
#include "../include/ext3.h"
#include "../include/heap.h"
#include "../include/kstring.h"

extern void kprintf(const char *fmt, ...);

#define JBD_MAGIC 0xC03B3998U

#define JBD_DESCRIPTOR_BLOCK 1U
#define JBD_COMMIT_BLOCK 2U
#define JBD_SUPERBLOCK_V1 3U
#define JBD_SUPERBLOCK_V2 4U
#define JBD_REVOKE_BLOCK 5U

#define JBD_FLAG_ESCAPE 1U
#define JBD_FLAG_SAME_UUID 2U
#define JBD_FLAG_LAST_TAG 8U

#define JBD_INCOMPAT_REVOKE 0x1U
#define JBD_INCOMPAT_KNOWN JBD_INCOMPAT_REVOKE

#define EXT3_MAX_BLOCK_SIZE 4096U
#define EXT3_HEADER_BYTES 12U
#define EXT3_TAG_BYTES 8U
#define EXT3_UUID_BYTES 16U
#define EXT3_STAGE_LIMIT 256U

struct revoke_entry {
    uint32_t block;
    uint32_t sequence;
};

struct ext3_journal {
    struct ext3_journal_ops ops;
    void *context;
    uint32_t block_size;
    int busy;

    uint32_t length;
    uint32_t first;
    uint32_t sequence;
    uint32_t start;

    uint32_t *mapped;
    uint32_t mapped_count;

    uint8_t *stage_data;
    uint32_t *stage_block;
    uint32_t stage_count;
    uint32_t stage_capacity;

    struct revoke_entry *revokes;
    uint32_t revoke_count;
    uint32_t revoke_capacity;
};

static uint8_t work[EXT3_MAX_BLOCK_SIZE];
static uint8_t scratch[EXT3_MAX_BLOCK_SIZE];

static uint32_t swap32(uint32_t value) {
    return ((value & 0x000000FFU) << 24) | ((value & 0x0000FF00U) << 8) |
           ((value & 0x00FF0000U) >> 8) | ((value & 0xFF000000U) >> 24);
}

static uint32_t load_be32(const void *at) {
    uint32_t value;
    memcpy(&value, at, sizeof(value));
    return swap32(value);
}

static void store_be32(void *at, uint32_t value) {
    uint32_t encoded = swap32(value);
    memcpy(at, &encoded, sizeof(encoded));
}

static uint16_t load_be16(const void *at) {
    const uint8_t *bytes = (const uint8_t *)at;
    return (uint16_t)((uint16_t)bytes[0] << 8 | bytes[1]);
}

static void store_be16(void *at, uint16_t value) {
    uint8_t *bytes = (uint8_t *)at;
    bytes[0] = (uint8_t)(value >> 8);
    bytes[1] = (uint8_t)value;
}

static int needs_escape(const void *data) {
    const uint8_t *bytes = (const uint8_t *)data;
    return bytes[0] == 0xC0U && bytes[1] == 0x3BU &&
           bytes[2] == 0x39U && bytes[3] == 0x98U;
}

static uint8_t *staged_at(const struct ext3_journal *journal, uint32_t index) {
    return journal->stage_data + (size_t)index * journal->block_size;
}

static uint32_t step(const struct ext3_journal *journal, uint32_t position) {
    position++;
    if (position >= journal->length) position = journal->first;
    return position;
}

static int log_locate(struct ext3_journal *journal, uint32_t file_block,
                      uint32_t *disk) {
    if (file_block < journal->mapped_count && journal->mapped[file_block]) {
        *disk = journal->mapped[file_block];
        return 0;
    }
    if (journal->ops.map(journal->context, file_block, disk) != 0 || !*disk)
        return -1;
    return 0;
}

static int log_read(struct ext3_journal *journal, uint32_t file_block, void *out) {
    uint32_t disk;
    if (log_locate(journal, file_block, &disk) != 0) return -1;
    return journal->ops.read(journal->context, disk, out);
}

static int log_write(struct ext3_journal *journal, uint32_t file_block,
                     const void *data) {
    uint32_t disk;
    if (log_locate(journal, file_block, &disk) != 0) return -1;
    return journal->ops.write(journal->context, disk, data);
}

static int device_flush(struct ext3_journal *journal) {
    return journal->ops.flush ? journal->ops.flush(journal->context) : 0;
}

static void put_header(void *at, uint32_t type, uint32_t sequence) {
    store_be32((uint8_t *)at + 0, JBD_MAGIC);
    store_be32((uint8_t *)at + 4, type);
    store_be32((uint8_t *)at + 8, sequence);
}

static int write_journal_superblock(struct ext3_journal *journal, uint32_t start,
                                    uint32_t sequence) {
    if (log_read(journal, 0, work) != 0) return -1;
    store_be32(work + 24, sequence);
    store_be32(work + 28, start);
    if (log_write(journal, 0, work) != 0) return -1;
    journal->start = start;
    journal->sequence = sequence;
    return 0;
}

static void revokes_clear(struct ext3_journal *journal) {
    kfree(journal->revokes);
    journal->revokes = NULL;
    journal->revoke_count = 0;
    journal->revoke_capacity = 0;
}

void ext3_journal_close(struct ext3_journal *journal) {
    if (!journal) return;
    revokes_clear(journal);
    kfree(journal->mapped);
    kfree(journal->stage_data);
    kfree(journal->stage_block);
    kfree(journal);
}

int ext3_journal_active(const struct ext3_journal *journal) {
    return journal && !journal->busy;
}

uint32_t ext3_journal_length(const struct ext3_journal *journal) {
    return journal ? journal->length : 0;
}

uint32_t ext3_journal_staged(const struct ext3_journal *journal) {
    return journal ? journal->stage_count : 0;
}

static int write_run(struct ext3_journal *journal, uint32_t block, uint32_t count,
                     const void *data) {
    if (journal->ops.write_run)
        return journal->ops.write_run(journal->context, block, count, data);
    const uint8_t *bytes = (const uint8_t *)data;
    for (uint32_t index = 0; index < count; index++) {
        if (journal->ops.write(journal->context, block + index,
                               bytes + (size_t)index * journal->block_size) != 0)
            return -1;
    }
    return 0;
}

struct ext3_journal *ext3_journal_open(const struct ext3_journal_ops *ops,
                                       void *context, uint32_t block_size) {
    if (!ops || !ops->read || !ops->write || !ops->map ||
        block_size < 1024U || block_size > EXT3_MAX_BLOCK_SIZE) return NULL;
    struct ext3_journal *journal = (struct ext3_journal *)kmalloc(sizeof(*journal));
    if (!journal) return NULL;
    memset(journal, 0, sizeof(*journal));
    journal->ops = *ops;
    journal->context = context;
    journal->block_size = block_size;

    if (log_read(journal, 0, work) != 0 || load_be32(work + 0) != JBD_MAGIC)
        goto fail;
    uint32_t type = load_be32(work + 4);
    if (type != JBD_SUPERBLOCK_V1 && type != JBD_SUPERBLOCK_V2) goto fail;
    if (load_be32(work + 12) != block_size) goto fail;
    if (type == JBD_SUPERBLOCK_V2 && (load_be32(work + 40) & ~JBD_INCOMPAT_KNOWN))
        goto fail;

    journal->length = load_be32(work + 16);
    journal->first = load_be32(work + 20);
    journal->sequence = load_be32(work + 24);
    journal->start = load_be32(work + 28);
    if (journal->length < 8U || !journal->first || journal->first >= journal->length)
        goto fail;
    if (!journal->sequence) journal->sequence = 1U;

    uint32_t capacity = (block_size - EXT3_HEADER_BYTES) / EXT3_TAG_BYTES;
    if (capacity > EXT3_STAGE_LIMIT) capacity = EXT3_STAGE_LIMIT;
    if (capacity + 2U > journal->length - journal->first)
        capacity = journal->length - journal->first - 2U;
    uint32_t wanted = journal->first + capacity + 2U;
    journal->mapped = (uint32_t *)kmalloc(wanted * sizeof(uint32_t));
    journal->stage_block = (uint32_t *)kmalloc(capacity * sizeof(uint32_t));
    journal->stage_data = (uint8_t *)kmalloc((size_t)capacity * block_size);
    if (!journal->mapped || !journal->stage_block || !journal->stage_data) goto fail;
    journal->stage_capacity = capacity;

    for (uint32_t index = 0; index < wanted; index++) {
        uint32_t disk = 0;
        if (ops->map(context, index, &disk) != 0 || !disk) break;
        journal->mapped[index] = disk;
        journal->mapped_count = index + 1U;
    }
    if (journal->mapped_count < wanted) goto fail;
    return journal;

fail:
    ext3_journal_close(journal);
    return NULL;
}

static struct revoke_entry *revoke_slot(struct revoke_entry *table, uint32_t capacity,
                                        uint32_t block) {
    uint32_t mask = capacity - 1U;
    uint32_t at = (block * 2654435761U) & mask;
    while (table[at].block && table[at].block != block) at = (at + 1U) & mask;
    return &table[at];
}

static int revoke_remember(struct ext3_journal *journal, uint32_t block,
                           uint32_t sequence) {
    if (!block) return 0;
    if ((journal->revoke_count + 1U) * 2U > journal->revoke_capacity) {
        uint32_t capacity = journal->revoke_capacity ? journal->revoke_capacity * 2U : 256U;
        struct revoke_entry *table =
            (struct revoke_entry *)kmalloc(capacity * sizeof(*table));
        if (!table) return -1;
        memset(table, 0, capacity * sizeof(*table));
        for (uint32_t index = 0; index < journal->revoke_capacity; index++)
            if (journal->revokes[index].block)
                *revoke_slot(table, capacity, journal->revokes[index].block) =
                    journal->revokes[index];
        kfree(journal->revokes);
        journal->revokes = table;
        journal->revoke_capacity = capacity;
    }
    struct revoke_entry *entry =
        revoke_slot(journal->revokes, journal->revoke_capacity, block);
    if (!entry->block) {
        entry->block = block;
        entry->sequence = sequence;
        journal->revoke_count++;
    } else if (entry->sequence < sequence) {
        entry->sequence = sequence;
    }
    return 0;
}

static int revoked(const struct ext3_journal *journal, uint32_t block,
                   uint32_t sequence) {
    if (!journal->revoke_capacity || !block) return 0;
    struct revoke_entry *entry =
        revoke_slot(journal->revokes, journal->revoke_capacity, block);
    return entry->block == block && entry->sequence >= sequence;
}

static int collect_revokes(struct ext3_journal *journal, const uint8_t *block,
                           uint32_t sequence) {
    uint32_t used = load_be32(block + EXT3_HEADER_BYTES);
    if (used < EXT3_HEADER_BYTES + 4U || used > journal->block_size) return -1;
    for (uint32_t at = EXT3_HEADER_BYTES + 4U; at + 4U <= used; at += 4U) {
        if (revoke_remember(journal, load_be32(block + at), sequence) != 0) return -1;
    }
    return 0;
}

static int walk_log(struct ext3_journal *journal, uint32_t *committed_out, int replay) {
    uint32_t position = journal->start;
    uint32_t sequence = journal->sequence;
    uint32_t committed = journal->sequence;
    uint32_t guard = journal->length + 1U;

    while (guard--) {
        if (log_read(journal, position, work) != 0) return -1;
        if (load_be32(work + 0) != JBD_MAGIC) break;
        if (load_be32(work + 8) != sequence) break;
        uint32_t type = load_be32(work + 4);

        if (type == JBD_COMMIT_BLOCK) {
            sequence++;
            committed = sequence;
            position = step(journal, position);
            continue;
        }
        if (type == JBD_REVOKE_BLOCK) {
            if (!replay && collect_revokes(journal, work, sequence) != 0) return -1;
            position = step(journal, position);
            continue;
        }
        if (type != JBD_DESCRIPTOR_BLOCK) break;

        uint32_t at = EXT3_HEADER_BYTES;
        uint32_t tags = 0;
        uint32_t data_position = step(journal, position);
        int last = 0;
        while (!last && at + EXT3_TAG_BYTES <= journal->block_size) {
            uint32_t target = load_be32(work + at);
            uint16_t flags = load_be16(work + at + 6U);
            at += EXT3_TAG_BYTES;
            if (!(flags & JBD_FLAG_SAME_UUID)) at += EXT3_UUID_BYTES;
            last = (flags & JBD_FLAG_LAST_TAG) != 0;
            tags++;
            if (replay && sequence < *committed_out && !revoked(journal, target, sequence)) {
                if (log_read(journal, data_position, scratch) != 0) return -1;
                if (flags & JBD_FLAG_ESCAPE) store_be32(scratch, JBD_MAGIC);
                if (journal->ops.write(journal->context, target, scratch) != 0) return -1;
            }
            data_position = step(journal, data_position);
        }
        if (!tags) return -1;
        position = data_position;
    }

    if (!replay) *committed_out = committed;
    return 0;
}

int ext3_journal_recover(struct ext3_journal *journal) {
    if (!journal) return -1;
    if (!journal->start) return 0;

    journal->busy = 1;
    revokes_clear(journal);
    uint32_t committed = journal->sequence;
    int status = walk_log(journal, &committed, 0);
    if (status == 0 && committed != journal->sequence) {
        kprintf("EXT3: replaying the journal from transaction %u\n",
                (unsigned)journal->sequence);
        status = walk_log(journal, &committed, 1);
        if (status == 0) status = device_flush(journal);
    }
    if (status == 0) status = write_journal_superblock(journal, 0, committed);
    if (status == 0) status = device_flush(journal);
    revokes_clear(journal);
    journal->busy = 0;
    return status;
}

int ext3_journal_peek(const struct ext3_journal *journal, uint32_t block, void *out) {
    if (!journal) return -1;
    for (uint32_t index = 0; index < journal->stage_count; index++) {
        if (journal->stage_block[index] != block) continue;
        memcpy(out, staged_at(journal, index), journal->block_size);
        return 0;
    }
    return -1;
}

int ext3_journal_stage(struct ext3_journal *journal, uint32_t block, const void *data) {
    if (!journal || journal->busy) return -1;
    for (uint32_t index = 0; index < journal->stage_count; index++) {
        if (journal->stage_block[index] != block) continue;
        memcpy(staged_at(journal, index), data, journal->block_size);
        return 0;
    }
    if (journal->stage_count == journal->stage_capacity &&
        ext3_journal_commit(journal) != 0) return -1;
    journal->stage_block[journal->stage_count] = block;
    memcpy(staged_at(journal, journal->stage_count), data, journal->block_size);
    journal->stage_count++;
    return 0;
}

int ext3_journal_commit(struct ext3_journal *journal) {
    if (!journal || journal->busy) return -1;
    uint32_t count = journal->stage_count;
    if (!count) return 0;

    journal->busy = 1;
    uint32_t block_size = journal->block_size;
    uint32_t sequence = journal->sequence;
    uint32_t position = journal->first;
    int status = -1;

    memset(work, 0, block_size);
    put_header(work, JBD_DESCRIPTOR_BLOCK, sequence);
    for (uint32_t index = 0; index < count; index++) {
        uint8_t *tag = work + EXT3_HEADER_BYTES + index * EXT3_TAG_BYTES;
        uint16_t flags = JBD_FLAG_SAME_UUID;
        if (needs_escape(staged_at(journal, index))) flags |= JBD_FLAG_ESCAPE;
        if (index + 1U == count) flags |= JBD_FLAG_LAST_TAG;
        store_be32(tag, journal->stage_block[index]);
        store_be16(tag + 4U, 0);
        store_be16(tag + 6U, flags);
    }
    if (log_write(journal, position, work) != 0) goto done;
    position = step(journal, position);

    for (uint32_t index = 0; index < count; ) {
        if (needs_escape(staged_at(journal, index))) {
            memcpy(scratch, staged_at(journal, index), block_size);
            store_be32(scratch, 0);
            if (log_write(journal, position, scratch) != 0) goto done;
            position = step(journal, position);
            index++;
            continue;
        }
        uint32_t run = 1;
        while (index + run < count &&
               !needs_escape(staged_at(journal, index + run)) &&
               position + run < journal->mapped_count &&
               journal->mapped[position + run] == journal->mapped[position] + run) run++;
        if (write_run(journal, journal->mapped[position], run,
                      staged_at(journal, index)) != 0) goto done;
        position += run;
        if (position >= journal->length) position = journal->first;
        index += run;
    }

    memset(work, 0, block_size);
    put_header(work, JBD_COMMIT_BLOCK, sequence);
    if (log_write(journal, position, work) != 0) goto done;
    if (device_flush(journal) != 0) goto done;

    if (journal->ops.mark && journal->ops.mark(journal->context, 1) != 0) goto done;
    if (write_journal_superblock(journal, journal->first, sequence) != 0) goto done;
    if (device_flush(journal) != 0) goto done;

    for (uint32_t index = 0; index < count; ) {
        uint32_t run = 1;
        while (index + run < count &&
               journal->stage_block[index + run] == journal->stage_block[index] + run) run++;
        if (write_run(journal, journal->stage_block[index], run,
                      staged_at(journal, index)) != 0) goto done;
        index += run;
    }
    if (device_flush(journal) != 0) goto done;

    if (write_journal_superblock(journal, 0, sequence + 1U) != 0) goto done;
    if (journal->ops.mark && journal->ops.mark(journal->context, 0) != 0) goto done;
    if (device_flush(journal) != 0) goto done;

    journal->stage_count = 0;
    status = 0;

done:
    journal->busy = 0;
    if (status != 0) kprintf("EXT3: journal commit failed\n");
    return status;
}
