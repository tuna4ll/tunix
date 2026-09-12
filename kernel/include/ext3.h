#ifndef TUNIX_EXT3_H
#define TUNIX_EXT3_H

#include <stdint.h>

struct ext3_journal_ops {
    int (*read)(uint32_t block, void *out);
    int (*write)(uint32_t block, const void *data);
    int (*map)(uint32_t file_block, uint32_t *disk_block);
    int (*flush)(void);
    int (*mark)(int needs_recovery);
};

int ext3_journal_attach(const struct ext3_journal_ops *provided);
void ext3_journal_detach(void);
int ext3_journal_present(void);
int ext3_journal_active(void);
int ext3_journal_recover(void);
int ext3_journal_stage(uint32_t block, const void *data);
int ext3_journal_peek(uint32_t block, void *out);
int ext3_journal_commit(void);
uint32_t ext3_journal_length(void);

#endif
