#ifndef TUNIX_ABI_GAPS_H
#define TUNIX_ABI_GAPS_H

#include <stdint.h>

#define ABI_GAP_SLOTS 32U
#define ABI_GAP_NAME_SIZE 32U

struct abi_gap {
    uint64_t syscall_number;
    uint64_t count;
    uint64_t last_pid;
    char last_name[ABI_GAP_NAME_SIZE];
};

void abi_gaps_note(uint64_t syscall_number);
int abi_gaps_snapshot(unsigned index, struct abi_gap *out);
uint64_t abi_gaps_overflow(void);
void abi_gaps_clear(void);

#endif
