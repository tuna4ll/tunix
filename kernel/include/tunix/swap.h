#ifndef TUNIX_SWAP_H
#define TUNIX_SWAP_H

#include <stddef.h>
#include <stdint.h>

struct vfs_node;

#define SWAP_AREAS        32U
#define SWAP_OFFSET_BITS  35U
#define SWAP_FLAG_PREFER  0x8000U
#define SWAP_FLAG_PRIO    0x7FFFU
#define SWAP_FLAG_DISCARD 0x70000U

int swap_activate(struct vfs_node *node, const char *path, int flags);
int swap_deactivate(struct vfs_node *node);

uint64_t swap_total_pages(void);
uint64_t swap_free_pages(void);
uint64_t swap_cached_pages(void);

int swap_entry_dup(uint64_t slot);
void swap_entry_free(uint64_t slot);

uint64_t swap_cache_take(uint64_t physical);
int swap_install_decision(uint64_t slot, uint64_t physical, uint64_t *flags);

int swap_fault_in(uint64_t cr3, uint64_t address);
unsigned swap_writeback(unsigned budget);
int swap_reserve_entries(unsigned wanted);

typedef void (*swap_report_fn)(void *context, const char *path, int is_file, uint64_t size_kb,
                               uint64_t used_kb, int priority);
void swap_report(swap_report_fn report, void *context);

#endif
