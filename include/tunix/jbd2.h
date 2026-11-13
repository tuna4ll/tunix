#ifndef TUNIX_JBD2_H
#define TUNIX_JBD2_H

#include <stdint.h>

struct jbd2_journal;

struct jbd2_journal_ops {
    int (*read)(void *context, uint64_t block, void *out);
    int (*write)(void *context, uint64_t block, const void *data);
    int (*map)(void *context, uint32_t file_block, uint64_t *disk_block);
    int (*flush)(void *context);
    int (*write_run)(void *context, uint64_t block, uint32_t count, const void *data);
};

#define JBD2_WANT_64BIT 0x1U
#define JBD2_WANT_CSUM  0x2U

struct jbd2_journal *jbd2_journal_open(const struct jbd2_journal_ops *ops, void *context,
                                       uint32_t block_size);
void jbd2_journal_close(struct jbd2_journal *journal);
int jbd2_journal_recover(struct jbd2_journal *journal);
int jbd2_journal_begin(struct jbd2_journal *journal, uint32_t wanted);
int jbd2_journal_end(struct jbd2_journal *journal);
int jbd2_journal_commit(struct jbd2_journal *journal, uint32_t count, const uint64_t *targets,
                        uint8_t *const *data);
uint32_t jbd2_journal_capacity(const struct jbd2_journal *journal);
int jbd2_journal_pending_fast_commit(const struct jbd2_journal *journal);

#endif
