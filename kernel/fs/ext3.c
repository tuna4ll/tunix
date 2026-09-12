#include <stddef.h>
#include <stdint.h>
#include "../include/ext3.h"
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

#define JBD_INCOMPAT_KNOWN 0U

#define EXT3_BLOCK_SIZE 4096U
#define EXT3_HEADER_BYTES 12U
#define EXT3_TAG_BYTES 8U
#define EXT3_UUID_BYTES 16U

#define EXT3_STAGE_MAX 48U
#define EXT3_MAPPED_MAX (EXT3_STAGE_MAX + 4U)
#define EXT3_REVOKE_MAX 512U

static struct ext3_journal_ops ops;
static int journal_ready;
static int journal_busy;

static uint32_t journal_length;
static uint32_t journal_first;
static uint32_t journal_sequence;
static uint32_t journal_start;

static uint32_t mapped[EXT3_MAPPED_MAX];
static uint32_t mapped_count;

static uint8_t stage_data[EXT3_STAGE_MAX][EXT3_BLOCK_SIZE];
static uint32_t stage_block[EXT3_STAGE_MAX];
static uint32_t stage_count;

static uint32_t revoke_block[EXT3_REVOKE_MAX];
static uint32_t revoke_sequence[EXT3_REVOKE_MAX];
static uint32_t revoke_count;

static uint8_t work[EXT3_BLOCK_SIZE];
static uint8_t scratch[EXT3_BLOCK_SIZE];

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

static uint32_t step(uint32_t position) {
    position++;
    if (position >= journal_length) position = journal_first;
    return position;
}

static int log_read(uint32_t file_block, void *out) {
    uint32_t disk;
    if (file_block < mapped_count && mapped[file_block]) {
        return ops.read(mapped[file_block], out);
    }
    if (!ops.map || ops.map(file_block, &disk) != 0 || !disk) return -1;
    return ops.read(disk, out);
}

static int log_write(uint32_t file_block, const void *data) {
    uint32_t disk;
    if (file_block < mapped_count && mapped[file_block]) {
        return ops.write(mapped[file_block], data);
    }
    if (!ops.map || ops.map(file_block, &disk) != 0 || !disk) return -1;
    return ops.write(disk, data);
}

static void put_header(void *at, uint32_t type, uint32_t sequence) {
    store_be32((uint8_t *)at + 0, JBD_MAGIC);
    store_be32((uint8_t *)at + 4, type);
    store_be32((uint8_t *)at + 8, sequence);
}

static int write_journal_superblock(uint32_t start, uint32_t sequence) {
    if (log_read(0, work) != 0) return -1;
    store_be32(work + 24, sequence);
    store_be32(work + 28, start);
    if (log_write(0, work) != 0) return -1;
    journal_start = start;
    journal_sequence = sequence;
    return 0;
}

void ext3_journal_detach(void) {
    journal_ready = 0;
    journal_busy = 0;
    stage_count = 0;
    revoke_count = 0;
    mapped_count = 0;
}

int ext3_journal_present(void) { return journal_ready; }

int ext3_journal_active(void) { return journal_ready && !journal_busy; }

uint32_t ext3_journal_length(void) { return journal_length; }

int ext3_journal_attach(const struct ext3_journal_ops *provided) {
    ext3_journal_detach();
    if (!provided || !provided->read || !provided->write || !provided->map)
        return -1;
    ops = *provided;

    if (log_read(0, work) != 0) return -1;
    if (load_be32(work + 0) != JBD_MAGIC) return -1;
    uint32_t type = load_be32(work + 4);
    if (type != JBD_SUPERBLOCK_V1 && type != JBD_SUPERBLOCK_V2) return -1;
    if (load_be32(work + 12) != EXT3_BLOCK_SIZE) return -1;
    if (load_be32(work + 40) & ~JBD_INCOMPAT_KNOWN) return -1;

    journal_length = load_be32(work + 16);
    journal_first = load_be32(work + 20);
    journal_sequence = load_be32(work + 24);
    journal_start = load_be32(work + 28);
    if (journal_length < 8U || !journal_first || journal_first >= journal_length)
        return -1;
    if (!journal_sequence) journal_sequence = 1U;

    mapped_count = 0;
    for (uint32_t index = 0; index < EXT3_MAPPED_MAX && index < journal_length; index++) {
        uint32_t disk = 0;
        if (ops.map(index, &disk) != 0 || !disk) break;
        mapped[index] = disk;
        mapped_count = index + 1U;
    }
    if (mapped_count <= journal_first) return -1;

    journal_ready = 1;
    return 0;
}

static int revoke_remember(uint32_t block, uint32_t sequence) {
    for (uint32_t index = 0; index < revoke_count; index++) {
        if (revoke_block[index] != block) continue;
        if (revoke_sequence[index] < sequence) revoke_sequence[index] = sequence;
        return 0;
    }
    if (revoke_count == EXT3_REVOKE_MAX) return -1;
    revoke_block[revoke_count] = block;
    revoke_sequence[revoke_count] = sequence;
    revoke_count++;
    return 0;
}

static int revoked(uint32_t block, uint32_t sequence) {
    for (uint32_t index = 0; index < revoke_count; index++) {
        if (revoke_block[index] == block && revoke_sequence[index] >= sequence)
            return 1;
    }
    return 0;
}

static int collect_revokes(const uint8_t *block, uint32_t sequence) {
    uint32_t used = load_be32(block + EXT3_HEADER_BYTES);
    if (used < EXT3_HEADER_BYTES + 4U || used > EXT3_BLOCK_SIZE) return -1;
    for (uint32_t at = EXT3_HEADER_BYTES + 4U; at + 4U <= used; at += 4U) {
        if (revoke_remember(load_be32(block + at), sequence) != 0) return -1;
    }
    return 0;
}

static int walk_log(uint32_t *committed_out, int replay) {
    uint32_t position = journal_start;
    uint32_t sequence = journal_sequence;
    uint32_t committed = journal_sequence;
    uint32_t guard = journal_length + 1U;

    while (guard--) {
        if (log_read(position, work) != 0) return -1;
        if (load_be32(work + 0) != JBD_MAGIC) break;
        if (load_be32(work + 8) != sequence) break;
        uint32_t type = load_be32(work + 4);

        if (type == JBD_COMMIT_BLOCK) {
            sequence++;
            committed = sequence;
            position = step(position);
            continue;
        }
        if (type == JBD_REVOKE_BLOCK) {
            if (!replay && collect_revokes(work, sequence) != 0) return -1;
            position = step(position);
            continue;
        }
        if (type != JBD_DESCRIPTOR_BLOCK) break;

        uint32_t at = EXT3_HEADER_BYTES;
        uint32_t tags = 0;
        uint32_t data_position = step(position);
        int last = 0;
        while (!last && at + EXT3_TAG_BYTES <= EXT3_BLOCK_SIZE) {
            uint32_t target = load_be32(work + at);
            uint16_t flags = load_be16(work + at + 6U);
            at += EXT3_TAG_BYTES;
            if (!(flags & JBD_FLAG_SAME_UUID)) at += EXT3_UUID_BYTES;
            last = (flags & JBD_FLAG_LAST_TAG) != 0;
            tags++;
            if (replay && sequence < *committed_out && !revoked(target, sequence)) {
                if (log_read(data_position, scratch) != 0) return -1;
                if (flags & JBD_FLAG_ESCAPE) store_be32(scratch, JBD_MAGIC);
                if (ops.write(target, scratch) != 0) return -1;
            }
            data_position = step(data_position);
        }
        if (!tags) return -1;
        position = data_position;
    }

    if (!replay) *committed_out = committed;
    return 0;
}

int ext3_journal_recover(void) {
    if (!journal_ready) return -1;
    if (!journal_start) return 0;

    journal_busy = 1;
    revoke_count = 0;
    uint32_t committed = journal_sequence;
    int status = walk_log(&committed, 0);
    if (status == 0 && committed != journal_sequence) {
        kprintf("EXT3: replaying the journal from transaction %u\n",
                (unsigned)journal_sequence);
        status = walk_log(&committed, 1);
        if (status == 0 && ops.flush) status = ops.flush();
    }
    if (status == 0) status = write_journal_superblock(0, committed);
    if (status == 0 && ops.flush) status = ops.flush();
    revoke_count = 0;
    journal_busy = 0;
    return status;
}

int ext3_journal_peek(uint32_t block, void *out) {
    if (!journal_ready) return -1;
    for (uint32_t index = 0; index < stage_count; index++) {
        if (stage_block[index] != block) continue;
        memcpy(out, stage_data[index], EXT3_BLOCK_SIZE);
        return 0;
    }
    return -1;
}

int ext3_journal_stage(uint32_t block, const void *data) {
    if (!journal_ready || journal_busy) return -1;
    for (uint32_t index = 0; index < stage_count; index++) {
        if (stage_block[index] != block) continue;
        memcpy(stage_data[index], data, EXT3_BLOCK_SIZE);
        return 0;
    }
    if (stage_count == EXT3_STAGE_MAX && ext3_journal_commit() != 0) return -1;
    stage_block[stage_count] = block;
    memcpy(stage_data[stage_count], data, EXT3_BLOCK_SIZE);
    stage_count++;
    return 0;
}

int ext3_journal_commit(void) {
    if (!journal_ready || journal_busy) return -1;
    if (!stage_count) return 0;
    if (stage_count + 2U > mapped_count - journal_first) return -1;

    journal_busy = 1;
    uint32_t sequence = journal_sequence;
    uint32_t position = journal_first;
    int status = -1;

    memset(work, 0, sizeof(work));
    put_header(work, JBD_DESCRIPTOR_BLOCK, sequence);
    for (uint32_t index = 0; index < stage_count; index++) {
        uint8_t *tag = work + EXT3_HEADER_BYTES + index * EXT3_TAG_BYTES;
        uint16_t flags = JBD_FLAG_SAME_UUID;
        if (needs_escape(stage_data[index])) flags |= JBD_FLAG_ESCAPE;
        if (index + 1U == stage_count) flags |= JBD_FLAG_LAST_TAG;
        store_be32(tag, stage_block[index]);
        store_be16(tag + 4U, 0);
        store_be16(tag + 6U, flags);
    }
    if (log_write(position, work) != 0) goto done;
    position = step(position);

    for (uint32_t index = 0; index < stage_count; index++) {
        memcpy(scratch, stage_data[index], EXT3_BLOCK_SIZE);
        if (needs_escape(scratch)) store_be32(scratch, 0);
        if (log_write(position, scratch) != 0) goto done;
        position = step(position);
    }

    memset(work, 0, sizeof(work));
    put_header(work, JBD_COMMIT_BLOCK, sequence);
    if (log_write(position, work) != 0) goto done;
    if (ops.flush && ops.flush() != 0) goto done;

    if (ops.mark && ops.mark(1) != 0) goto done;
    if (write_journal_superblock(journal_first, sequence) != 0) goto done;
    if (ops.flush && ops.flush() != 0) goto done;

    for (uint32_t index = 0; index < stage_count; index++) {
        if (ops.write(stage_block[index], stage_data[index]) != 0) goto done;
    }
    if (ops.flush && ops.flush() != 0) goto done;

    if (write_journal_superblock(0, sequence + 1U) != 0) goto done;
    if (ops.mark && ops.mark(0) != 0) goto done;
    if (ops.flush && ops.flush() != 0) goto done;

    stage_count = 0;
    status = 0;

done:
    journal_busy = 0;
    if (status != 0) kprintf("EXT3: journal commit failed\n");
    return status;
}
