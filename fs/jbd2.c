#include <stddef.h>
#include <stdint.h>
#include <tunix/crc.h>
#include <tunix/heap.h>
#include <tunix/jbd2.h>
#include <tunix/kstring.h>
#include <tunix/time.h>
#include <tunix/kprintf.h>

#define JBD2_MAGIC 0xC03B3998U

#define JBD2_DESCRIPTOR_BLOCK 1U
#define JBD2_COMMIT_BLOCK     2U
#define JBD2_SUPERBLOCK_V1    3U
#define JBD2_SUPERBLOCK_V2    4U
#define JBD2_REVOKE_BLOCK     5U

#define JBD2_FLAG_ESCAPE    1U
#define JBD2_FLAG_SAME_UUID 2U
#define JBD2_FLAG_LAST_TAG  8U

#define JBD2_COMPAT_CHECKSUM      0x1U
#define JBD2_INCOMPAT_REVOKE      0x1U
#define JBD2_INCOMPAT_64BIT       0x2U
#define JBD2_INCOMPAT_ASYNC       0x4U
#define JBD2_INCOMPAT_CSUM_V2     0x8U
#define JBD2_INCOMPAT_CSUM_V3     0x10U
#define JBD2_INCOMPAT_FAST_COMMIT 0x20U
#define JBD2_INCOMPAT_KNOWN \
    (JBD2_INCOMPAT_REVOKE | JBD2_INCOMPAT_64BIT | JBD2_INCOMPAT_ASYNC | JBD2_INCOMPAT_CSUM_V2 | \
     JBD2_INCOMPAT_CSUM_V3 | JBD2_INCOMPAT_FAST_COMMIT)

#define JBD2_CRC32_CHKSUM   1U
#define JBD2_CRC32C_CHKSUM  4U
#define JBD2_DEFAULT_FC     256U
#define JBD2_FC_TAG_HEAD    9U
#define JBD2_SB_CHECKSUM_AT 0xFCU

#define JBD2_MAX_BLOCK_SIZE 4096U
#define JBD2_HEADER_BYTES   12U
#define JBD2_UUID_BYTES     16U
#define JBD2_GATHER_BLOCKS  32U

#define PASS_SCAN   0
#define PASS_REVOKE 1
#define PASS_REPLAY 2

struct revoke_entry {
    uint64_t block;
    uint32_t sequence;
};

struct jbd2_journal {
    struct jbd2_journal_ops ops;
    void *context;
    uint32_t block_size;
    int busy;

    uint32_t maxlen;
    uint32_t first;
    uint32_t last;
    uint32_t sequence;
    uint32_t start;
    uint32_t compat;
    uint32_t incompat;
    uint8_t uuid[JBD2_UUID_BYTES];
    uint32_t csum_seed;
    int fast_commit_pending;

    uint64_t *mapped;
    uint32_t mapped_count;
    int open_for_writes;

    uint8_t *gather;
    uint64_t gather_start;
    uint32_t gather_count;

    struct revoke_entry *revokes;
    uint32_t revoke_count;
    uint32_t revoke_capacity;
};

static uint8_t work[JBD2_MAX_BLOCK_SIZE];
static uint8_t scratch[JBD2_MAX_BLOCK_SIZE];

static uint32_t load_be32(const void *at) {
    const uint8_t *bytes = (const uint8_t *)at;
    return (uint32_t)bytes[0] << 24 | (uint32_t)bytes[1] << 16 | (uint32_t)bytes[2] << 8 | bytes[3];
}

static void store_be32(void *at, uint32_t value) {
    uint8_t *bytes = (uint8_t *)at;
    bytes[0] = (uint8_t)(value >> 24);
    bytes[1] = (uint8_t)(value >> 16);
    bytes[2] = (uint8_t)(value >> 8);
    bytes[3] = (uint8_t)value;
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

static uint16_t load_le16(const void *at) {
    const uint8_t *bytes = (const uint8_t *)at;
    return (uint16_t)(bytes[0] | (uint16_t)bytes[1] << 8);
}

static uint32_t load_le32(const void *at) {
    const uint8_t *bytes = (const uint8_t *)at;
    return bytes[0] | (uint32_t)bytes[1] << 8 | (uint32_t)bytes[2] << 16 | (uint32_t)bytes[3] << 24;
}

static int has_csum(const struct jbd2_journal *journal) {
    return (journal->incompat & (JBD2_INCOMPAT_CSUM_V2 | JBD2_INCOMPAT_CSUM_V3)) != 0;
}

static int has_csum_v3(const struct jbd2_journal *journal) {
    return (journal->incompat & JBD2_INCOMPAT_CSUM_V3) != 0;
}

static int has_64bit(const struct jbd2_journal *journal) {
    return (journal->incompat & JBD2_INCOMPAT_64BIT) != 0;
}

static uint32_t tag_bytes(const struct jbd2_journal *journal) {
    if (has_csum_v3(journal)) return 16U;
    uint32_t size = 12U;
    if (journal->incompat & JBD2_INCOMPAT_CSUM_V2) size += 2U;
    return has_64bit(journal) ? size : size - 4U;
}

static uint32_t tail_bytes(const struct jbd2_journal *journal) {
    return has_csum(journal) ? 4U : 0U;
}

static uint32_t block_csum(const struct jbd2_journal *journal, uint8_t *block, uint32_t at) {
    uint32_t saved = load_be32(block + at);
    store_be32(block + at, 0);
    uint32_t sum = crc32c(journal->csum_seed, block, journal->block_size);
    store_be32(block + at, saved);
    return sum;
}

static uint32_t data_csum(const struct jbd2_journal *journal, uint32_t sequence,
                          const uint8_t *data) {
    uint8_t encoded[4];
    store_be32(encoded, sequence);
    uint32_t sum = crc32c(journal->csum_seed, encoded, sizeof(encoded));
    return crc32c(sum, data, journal->block_size);
}

static int needs_escape(const void *data) { return load_be32(data) == JBD2_MAGIC; }

static uint32_t step(const struct jbd2_journal *journal, uint32_t position) {
    position++;
    if (position >= journal->last) position = journal->first;
    return position;
}

static int log_locate(struct jbd2_journal *journal, uint32_t file_block, uint64_t *disk) {
    if (file_block < journal->mapped_count && journal->mapped[file_block]) {
        *disk = journal->mapped[file_block];
        return 0;
    }
    if (journal->ops.map(journal->context, file_block, disk) != 0 || !*disk) return -1;
    return 0;
}

static int log_read(struct jbd2_journal *journal, uint32_t file_block, void *out) {
    uint64_t disk;
    if (log_locate(journal, file_block, &disk) != 0) return -1;
    return journal->ops.read(journal->context, disk, out);
}

static int log_write(struct jbd2_journal *journal, uint32_t file_block, const void *data) {
    uint64_t disk;
    if (log_locate(journal, file_block, &disk) != 0) return -1;
    return journal->ops.write(journal->context, disk, data);
}

static int device_flush(struct jbd2_journal *journal) {
    return journal->ops.flush ? journal->ops.flush(journal->context) : 0;
}

static void put_header(void *at, uint32_t type, uint32_t sequence) {
    store_be32((uint8_t *)at + 0, JBD2_MAGIC);
    store_be32((uint8_t *)at + 4, type);
    store_be32((uint8_t *)at + 8, sequence);
}

static void seal_superblock(struct jbd2_journal *journal, uint8_t *block) {
    if (!has_csum(journal)) return;
    block[80] = (uint8_t)JBD2_CRC32C_CHKSUM;
    store_be32(block + JBD2_SB_CHECKSUM_AT, 0);
    store_be32(block + JBD2_SB_CHECKSUM_AT, crc32c(0xFFFFFFFFU, block, 1024));
}

static int write_journal_superblock(struct jbd2_journal *journal, uint32_t start,
                                    uint32_t sequence) {
    if (log_read(journal, 0, work) != 0) return -1;
    store_be32(work + 24, sequence);
    store_be32(work + 28, start);
    if (load_be32(work + 4) == JBD2_SUPERBLOCK_V2) {
        store_be32(work + 36, journal->compat);
        store_be32(work + 40, journal->incompat);
        seal_superblock(journal, work);
    }
    if (log_write(journal, 0, work) != 0) return -1;
    journal->start = start;
    journal->sequence = sequence;
    return 0;
}

static void revokes_clear(struct jbd2_journal *journal) {
    kfree(journal->revokes);
    journal->revokes = NULL;
    journal->revoke_count = 0;
    journal->revoke_capacity = 0;
}

void jbd2_journal_close(struct jbd2_journal *journal) {
    if (!journal) return;
    revokes_clear(journal);
    kfree(journal->mapped);
    kfree(journal->gather);
    kfree(journal);
}

static uint32_t tags_per_descriptor(const struct jbd2_journal *journal) {
    return (journal->block_size - JBD2_HEADER_BYTES - tail_bytes(journal)) / tag_bytes(journal);
}

uint32_t jbd2_journal_capacity(const struct jbd2_journal *journal) {
    if (!journal) return 0;
    uint32_t space = journal->last - journal->first;
    uint32_t tags = tags_per_descriptor(journal);
    uint32_t usable = space > 2U ? space - 2U : 0;
    return usable - (usable + tags) / (tags + 1U);
}

int jbd2_journal_pending_fast_commit(const struct jbd2_journal *journal) {
    return journal && journal->fast_commit_pending;
}

static int write_run(struct jbd2_journal *journal, uint64_t block, uint32_t count,
                     const void *data) {
    if (journal->ops.write_run) return journal->ops.write_run(journal->context, block, count, data);
    const uint8_t *bytes = (const uint8_t *)data;
    for (uint32_t index = 0; index < count; index++) {
        if (journal->ops.write(journal->context, block + index,
                               bytes + (size_t)index * journal->block_size) != 0)
            return -1;
    }
    return 0;
}

struct jbd2_journal *jbd2_journal_open(const struct jbd2_journal_ops *ops, void *context,
                                       uint32_t block_size) {
    if (!ops || !ops->read || !ops->write || !ops->map || block_size < 1024U ||
        block_size > JBD2_MAX_BLOCK_SIZE)
        return NULL;
    struct jbd2_journal *journal = (struct jbd2_journal *)kmalloc(sizeof(*journal));
    if (!journal) return NULL;
    memset(journal, 0, sizeof(*journal));
    journal->ops = *ops;
    journal->context = context;
    journal->block_size = block_size;

    uint64_t home;
    if (ops->map(context, 0, &home) != 0 || !home || ops->read(context, home, work) != 0 ||
        load_be32(work + 0) != JBD2_MAGIC)
        goto fail;
    uint32_t type = load_be32(work + 4);
    if (type != JBD2_SUPERBLOCK_V1 && type != JBD2_SUPERBLOCK_V2) goto fail;
    if (load_be32(work + 12) != block_size) goto fail;
    if (type == JBD2_SUPERBLOCK_V2) {
        journal->compat = load_be32(work + 36);
        journal->incompat = load_be32(work + 40);
        if (journal->incompat & ~JBD2_INCOMPAT_KNOWN) {
            kprintf("JBD2: unknown journal features %x\n",
                    (unsigned)(journal->incompat & ~JBD2_INCOMPAT_KNOWN));
            goto fail;
        }
        memcpy(journal->uuid, work + 48, JBD2_UUID_BYTES);
        if (has_csum(journal)) {
            if (work[80] != JBD2_CRC32C_CHKSUM) goto fail;
            uint32_t stored = load_be32(work + JBD2_SB_CHECKSUM_AT);
            store_be32(work + JBD2_SB_CHECKSUM_AT, 0);
            if (crc32c(0xFFFFFFFFU, work, 1024) != stored) {
                kprintf("JBD2: the journal superblock checksum is wrong\n");
                goto fail;
            }
        }
    }
    journal->csum_seed = crc32c(0xFFFFFFFFU, journal->uuid, JBD2_UUID_BYTES);

    journal->maxlen = load_be32(work + 16);
    journal->first = load_be32(work + 20);
    journal->sequence = load_be32(work + 24);
    journal->start = load_be32(work + 28);
    journal->last = journal->maxlen;
    if (journal->incompat & JBD2_INCOMPAT_FAST_COMMIT) {
        uint32_t fast = load_be32(work + 84);
        if (!fast) fast = JBD2_DEFAULT_FC;
        if (fast >= journal->maxlen) goto fail;
        journal->last = journal->maxlen - fast;
    }
    if (journal->last < 8U || !journal->first || journal->first + 4U >= journal->last) goto fail;
    if (!journal->sequence) journal->sequence = 1U;

    uint32_t wanted = journal->maxlen;
    journal->mapped = (uint64_t *)kmalloc(wanted * sizeof(uint64_t));
    journal->gather = (uint8_t *)kmalloc((size_t)JBD2_GATHER_BLOCKS * block_size);
    if (!journal->mapped || !journal->gather) goto fail;

    for (uint32_t index = 0; index < wanted; index++) {
        uint64_t disk = 0;
        if (ops->map(context, index, &disk) != 0 || !disk) break;
        journal->mapped[index] = disk;
        journal->mapped_count = index + 1U;
    }
    if (journal->mapped_count < wanted) goto fail;
    return journal;

fail:
    jbd2_journal_close(journal);
    return NULL;
}

static uint32_t revoke_hash(uint64_t block) {
    return (uint32_t)((block * 0x9E3779B97F4A7C15ULL) >> 32);
}

static struct revoke_entry *revoke_slot(struct revoke_entry *table, uint32_t capacity,
                                        uint64_t block) {
    uint32_t mask = capacity - 1U;
    uint32_t at = revoke_hash(block) & mask;
    while (table[at].block && table[at].block != block) at = (at + 1U) & mask;
    return &table[at];
}

static int revoke_remember(struct jbd2_journal *journal, uint64_t block, uint32_t sequence) {
    if (!block) return 0;
    if ((journal->revoke_count + 1U) * 2U > journal->revoke_capacity) {
        uint32_t capacity = journal->revoke_capacity ? journal->revoke_capacity * 2U : 256U;
        struct revoke_entry *table = (struct revoke_entry *)kmalloc(capacity * sizeof(*table));
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
    struct revoke_entry *entry = revoke_slot(journal->revokes, journal->revoke_capacity, block);
    if (!entry->block) {
        entry->block = block;
        entry->sequence = sequence;
        journal->revoke_count++;
    } else if ((int32_t)(entry->sequence - sequence) < 0) {
        entry->sequence = sequence;
    }
    return 0;
}

static int revoked(const struct jbd2_journal *journal, uint64_t block, uint32_t sequence) {
    if (!journal->revoke_capacity || !block) return 0;
    struct revoke_entry *entry = revoke_slot(journal->revokes, journal->revoke_capacity, block);
    return entry->block == block && (int32_t)(entry->sequence - sequence) >= 0;
}

static int collect_revokes(struct jbd2_journal *journal, uint8_t *block, uint32_t sequence) {
    if (has_csum(journal) &&
        block_csum(journal, block, journal->block_size - 4U) !=
            load_be32(block + journal->block_size - 4U)) {
        kprintf("JBD2: revoke block %u has a bad checksum\n", (unsigned)sequence);
        return -1;
    }
    uint32_t used = load_be32(block + JBD2_HEADER_BYTES);
    uint32_t record = has_64bit(journal) ? 8U : 4U;
    if (used < JBD2_HEADER_BYTES + 4U || used > journal->block_size - tail_bytes(journal))
        return -1;
    for (uint32_t at = JBD2_HEADER_BYTES + 4U; at + record <= used; at += record) {
        uint64_t target = record == 8U
            ? (uint64_t)load_be32(block + at) << 32 | load_be32(block + at + 4U)
            : load_be32(block + at);
        if (revoke_remember(journal, target, sequence) != 0) return -1;
    }
    return 0;
}

struct tag {
    uint64_t target;
    uint32_t flags;
    uint32_t checksum;
};

static uint32_t read_tag(const struct jbd2_journal *journal, const uint8_t *at, struct tag *out) {
    out->target = load_be32(at);
    if (has_csum_v3(journal)) {
        out->flags = load_be32(at + 4U);
        out->checksum = load_be32(at + 12U);
    } else {
        out->flags = load_be16(at + 6U);
        out->checksum = load_be16(at + 4U);
    }
    if (has_64bit(journal)) out->target |= (uint64_t)load_be32(at + 8U) << 32;
    uint32_t size = tag_bytes(journal);
    if (!(out->flags & JBD2_FLAG_SAME_UUID)) size += JBD2_UUID_BYTES;
    return size;
}

static int commit_valid(struct jbd2_journal *journal, uint8_t *block, uint32_t checksum) {
    if (has_csum(journal)) return block_csum(journal, block, 16U) == load_be32(block + 16U);
    if (!(journal->compat & JBD2_COMPAT_CHECKSUM)) return 1;
    if (block[12] != JBD2_CRC32_CHKSUM || block[13] != 4U) return 1;
    return load_be32(block + 16U) == checksum;
}

static int replay_block(struct jbd2_journal *journal, const struct tag *tag, uint32_t position,
                        uint32_t sequence) {
    if (log_read(journal, position, scratch) != 0) return -1;
    if (has_csum(journal)) {
        uint32_t sum = data_csum(journal, sequence, scratch);
        if (!has_csum_v3(journal)) sum &= 0xFFFFU;
        if (sum != tag->checksum) {
            kprintf("JBD2: skipping block %u of transaction %u, bad checksum\n",
                    (unsigned)tag->target, (unsigned)sequence);
            return 0;
        }
    }
    if (tag->flags & JBD2_FLAG_ESCAPE) store_be32(scratch, JBD2_MAGIC);
    return journal->ops.write(journal->context, tag->target, scratch);
}

static int walk_log(struct jbd2_journal *journal, int pass, uint32_t *end) {
    uint32_t position = journal->start;
    uint32_t sequence = journal->sequence;
    uint32_t guard = journal->last + 1U;
    uint32_t checksum = 0xFFFFFFFFU;
    int verify_v1 = !has_csum(journal) && (journal->compat & JBD2_COMPAT_CHECKSUM);

    while (guard--) {
        if (pass != PASS_SCAN && sequence == *end) break;
        if (log_read(journal, position, work) != 0) return -1;
        if (load_be32(work + 0) != JBD2_MAGIC || load_be32(work + 8) != sequence) break;
        uint32_t type = load_be32(work + 4);

        if (type == JBD2_COMMIT_BLOCK) {
            if (pass == PASS_SCAN && !commit_valid(journal, work, checksum)) break;
            checksum = 0xFFFFFFFFU;
            sequence++;
            position = step(journal, position);
            continue;
        }
        if (type == JBD2_REVOKE_BLOCK) {
            if (pass == PASS_REVOKE && collect_revokes(journal, work, sequence) != 0) return -1;
            position = step(journal, position);
            continue;
        }
        if (type != JBD2_DESCRIPTOR_BLOCK) break;
        if (has_csum(journal) &&
            block_csum(journal, work, journal->block_size - 4U) !=
                load_be32(work + journal->block_size - 4U))
            break;
        if (verify_v1 && pass == PASS_SCAN)
            checksum = crc32_be(checksum, work, journal->block_size);

        uint32_t limit = journal->block_size - tail_bytes(journal);
        uint32_t at = JBD2_HEADER_BYTES;
        uint32_t tags = 0;
        uint32_t data_position = step(journal, position);
        int last = 0;
        while (!last && at + tag_bytes(journal) <= limit) {
            struct tag tag;
            at += read_tag(journal, work + at, &tag);
            last = (tag.flags & JBD2_FLAG_LAST_TAG) != 0;
            tags++;
            if (pass == PASS_SCAN && verify_v1) {
                if (log_read(journal, data_position, scratch) != 0) return -1;
                checksum = crc32_be(checksum, scratch, journal->block_size);
            }
            if (pass == PASS_REPLAY && !revoked(journal, tag.target, sequence)) {
                if (replay_block(journal, &tag, data_position, sequence) != 0) return -1;
            }
            data_position = step(journal, data_position);
        }
        if (!tags) return -1;
        position = data_position;
    }

    if (pass == PASS_SCAN) *end = sequence;
    return 0;
}

static void check_fast_commit(struct jbd2_journal *journal, uint32_t next) {
    if (!(journal->incompat & JBD2_INCOMPAT_FAST_COMMIT)) return;
    if (log_read(journal, journal->last, scratch) != 0) return;
    if (load_le16(scratch) == JBD2_FC_TAG_HEAD && load_le16(scratch + 2) >= 8U &&
        load_le32(scratch + 8) == next)
        journal->fast_commit_pending = 1;
}

int jbd2_journal_recover(struct jbd2_journal *journal) {
    if (!journal) return -1;
    if (!journal->start) return 0;

    journal->busy = 1;
    revokes_clear(journal);
    uint32_t end = journal->sequence;
    int status = walk_log(journal, PASS_SCAN, &end);
    if (status == 0) check_fast_commit(journal, end);
    if (status == 0 && journal->fast_commit_pending) {
        kprintf("JBD2: the journal holds fast commits only Linux can replay\n");
        journal->busy = 0;
        return -1;
    }
    if (status == 0 && end != journal->sequence) {
        kprintf("JBD2: replaying transactions %u to %u\n", (unsigned)journal->sequence,
                (unsigned)(end - 1U));
        status = walk_log(journal, PASS_REVOKE, &end);
        if (status == 0) status = walk_log(journal, PASS_REPLAY, &end);
        if (status == 0) status = device_flush(journal);
    }
    if (status == 0) status = write_journal_superblock(journal, 0, end);
    if (status == 0) status = device_flush(journal);
    revokes_clear(journal);
    journal->busy = 0;
    return status;
}

int jbd2_journal_begin(struct jbd2_journal *journal, uint32_t wanted) {
    if (!journal || journal->busy) return -1;
    if (journal->open_for_writes) return 0;
    if (log_read(journal, 0, work) != 0) return -1;
    if (load_be32(work + 4) == JBD2_SUPERBLOCK_V2) {
        if (wanted & JBD2_WANT_64BIT) journal->incompat |= JBD2_INCOMPAT_64BIT;
        if (wanted & JBD2_WANT_CSUM) {
            journal->compat &= ~JBD2_COMPAT_CHECKSUM;
            journal->incompat &= ~JBD2_INCOMPAT_CSUM_V2;
            journal->incompat |= JBD2_INCOMPAT_CSUM_V3;
        }
    }
    if (write_journal_superblock(journal, journal->first, journal->sequence) != 0) return -1;
    if (device_flush(journal) != 0) return -1;
    journal->open_for_writes = 1;
    return 0;
}

int jbd2_journal_end(struct jbd2_journal *journal) {
    if (!journal || !journal->open_for_writes) return 0;
    if (write_journal_superblock(journal, 0, journal->sequence) != 0) return -1;
    if (device_flush(journal) != 0) return -1;
    journal->open_for_writes = 0;
    return 0;
}

static int gather_flush(struct jbd2_journal *journal) {
    if (!journal->gather_count) return 0;
    int status = write_run(journal, journal->gather_start, journal->gather_count, journal->gather);
    journal->gather_count = 0;
    return status;
}

static int gather_add(struct jbd2_journal *journal, uint64_t disk_block, const void *data) {
    if (journal->gather_count &&
        (disk_block != journal->gather_start + journal->gather_count ||
         journal->gather_count == JBD2_GATHER_BLOCKS) &&
        gather_flush(journal) != 0)
        return -1;
    if (!journal->gather_count) journal->gather_start = disk_block;
    memcpy(journal->gather + (size_t)journal->gather_count * journal->block_size, data,
           journal->block_size);
    journal->gather_count++;
    return 0;
}

static int write_log_block(struct jbd2_journal *journal, uint32_t *position, const void *data) {
    uint64_t disk;
    if (log_locate(journal, *position, &disk) != 0) return -1;
    if (gather_add(journal, disk, data) != 0) return -1;
    *position = step(journal, *position);
    return 0;
}

static void put_tag(const struct jbd2_journal *journal, uint8_t *tag, uint64_t target,
                    uint32_t flags, uint32_t sequence, const uint8_t *data) {
    store_be32(tag, (uint32_t)target);
    if (has_csum_v3(journal)) {
        store_be32(tag + 4U, flags);
        store_be32(tag + 8U, (uint32_t)(target >> 32));
        store_be32(tag + 12U, data_csum(journal, sequence, data));
        return;
    }
    store_be16(tag + 4U,
               has_csum(journal) ? (uint16_t)data_csum(journal, sequence, data) : (uint16_t)0);
    store_be16(tag + 6U, (uint16_t)flags);
    if (has_64bit(journal)) store_be32(tag + 8U, (uint32_t)(target >> 32));
}

static int log_transaction(struct jbd2_journal *journal, uint32_t count, const uint64_t *targets,
                           uint8_t *const *data) {
    uint32_t block_size = journal->block_size;
    uint32_t tags_per_block = tags_per_descriptor(journal);
    uint32_t sequence = journal->sequence;
    uint32_t position = journal->first;
    uint32_t checksum = 0xFFFFFFFFU;
    int sum_v1 = !has_csum(journal) && (journal->compat & JBD2_COMPAT_CHECKSUM);

    for (uint32_t done = 0; done < count;) {
        uint32_t batch = count - done;
        if (batch > tags_per_block) batch = tags_per_block;
        memset(work, 0, block_size);
        put_header(work, JBD2_DESCRIPTOR_BLOCK, sequence);
        for (uint32_t index = 0; index < batch; index++) {
            uint8_t *tag = work + JBD2_HEADER_BYTES + index * tag_bytes(journal);
            uint32_t flags = JBD2_FLAG_SAME_UUID;
            if (needs_escape(data[done + index])) flags |= JBD2_FLAG_ESCAPE;
            if (index + 1U == batch) flags |= JBD2_FLAG_LAST_TAG;
            put_tag(journal, tag, targets[done + index], flags, sequence, data[done + index]);
        }
        if (has_csum(journal))
            store_be32(work + block_size - 4U, block_csum(journal, work, block_size - 4U));
        if (sum_v1) checksum = crc32_be(checksum, work, block_size);
        if (write_log_block(journal, &position, work) != 0) return -1;
        for (uint32_t index = 0; index < batch; index++) {
            const uint8_t *source = data[done + index];
            if (needs_escape(source)) {
                memcpy(scratch, source, block_size);
                store_be32(scratch, 0);
                source = scratch;
            }
            if (sum_v1) checksum = crc32_be(checksum, source, block_size);
            if (write_log_block(journal, &position, source) != 0) return -1;
        }
        done += batch;
    }
    if (gather_flush(journal) != 0 || device_flush(journal) != 0) return -1;

    memset(work, 0, block_size);
    put_header(work, JBD2_COMMIT_BLOCK, sequence);
    uint64_t now = time_realtime_ns();
    uint64_t seconds = now / 1000000000ULL;
    store_be32(work + 48, (uint32_t)(seconds >> 32));
    store_be32(work + 52, (uint32_t)seconds);
    store_be32(work + 56, (uint32_t)(now % 1000000000ULL));
    if (has_csum(journal)) {
        store_be32(work + 16, block_csum(journal, work, 16U));
    } else if (sum_v1) {
        work[12] = (uint8_t)JBD2_CRC32_CHKSUM;
        work[13] = 4U;
        store_be32(work + 16, checksum);
    }
    if (log_write(journal, position, work) != 0) return -1;
    return device_flush(journal);
}

static int checkpoint(struct jbd2_journal *journal, uint32_t count, const uint64_t *targets,
                      uint8_t *const *data) {
    for (uint32_t index = 0; index < count; index++)
        if (gather_add(journal, targets[index], data[index]) != 0) return -1;
    if (gather_flush(journal) != 0 || device_flush(journal) != 0) return -1;
    if (write_journal_superblock(journal, journal->first, journal->sequence + 1U) != 0) return -1;
    return device_flush(journal);
}

int jbd2_journal_commit(struct jbd2_journal *journal, uint32_t count, const uint64_t *targets,
                        uint8_t *const *data) {
    if (!journal || journal->busy || !journal->open_for_writes) return -1;
    if (!count) return 0;
    uint32_t capacity = jbd2_journal_capacity(journal);
    if (!capacity) return -1;
    journal->busy = 1;
    int status = 0;
    for (uint32_t done = 0; done < count && status == 0;) {
        uint32_t batch = count - done;
        if (batch > capacity) batch = capacity;
        status = log_transaction(journal, batch, targets + done, data + done);
        if (status == 0) status = checkpoint(journal, batch, targets + done, data + done);
        done += batch;
    }
    journal->busy = 0;
    if (status != 0) kprintf("JBD2: journal commit failed\n");
    return status;
}
