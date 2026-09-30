#ifndef TUNIX_EXT3_H
#define TUNIX_EXT3_H

#include <stdint.h>

struct ext3_journal;

struct ext3_journal_ops {
    int (*read)(void *context, uint32_t block, void *out);
    int (*write)(void *context, uint32_t block, const void *data);
    int (*map)(void *context, uint32_t file_block, uint32_t *disk_block);
    int (*flush)(void *context);
    int (*mark)(void *context, int needs_recovery);
    int (*write_run)(void *context, uint32_t block, uint32_t count, const void *data);
};

struct ext3_journal *ext3_journal_open(const struct ext3_journal_ops *ops,
                                       void *context, uint32_t block_size);
void ext3_journal_close(struct ext3_journal *journal);
int ext3_journal_active(const struct ext3_journal *journal);
int ext3_journal_recover(struct ext3_journal *journal);
int ext3_journal_stage(struct ext3_journal *journal, uint32_t block, const void *data);
int ext3_journal_peek(const struct ext3_journal *journal, uint32_t block, void *out);
int ext3_journal_commit(struct ext3_journal *journal);
uint32_t ext3_journal_length(const struct ext3_journal *journal);
uint32_t ext3_journal_staged(const struct ext3_journal *journal);

#endif
