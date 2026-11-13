#ifndef TUNIX_RECLAIM_H
#define TUNIX_RECLAIM_H

#include <stdint.h>

uint64_t reclaim_min_pages(void);
uint64_t reclaim_low_pages(void);
uint64_t reclaim_high_pages(void);
uint64_t reclaim_pages(uint64_t wanted);
void reclaim_wake(void);
void reclaim_note_allocation(void);
void reclaim_start(void);
struct reclaim_stats {
    uint64_t reclaimed;
    uint64_t swapped_out;
    uint64_t dropped;
    uint64_t scanned;
    uint64_t aged;
    uint64_t skipped;
};

void reclaim_statistics(struct reclaim_stats *stats);

#endif
