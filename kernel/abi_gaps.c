#include <stddef.h>
#include <stdint.h>

#include "include/abi_gaps.h"
#include "include/kstring.h"
#include "include/process.h"

static struct abi_gap gaps[ABI_GAP_SLOTS];
static uint64_t overflow;

void abi_gaps_note(uint64_t syscall_number) {
    unsigned free_slot = ABI_GAP_SLOTS;
    for (unsigned index = 0; index < ABI_GAP_SLOTS; index++) {
        if (gaps[index].count && gaps[index].syscall_number == syscall_number) {
            free_slot = index;
            break;
        }
        if (!gaps[index].count && free_slot == ABI_GAP_SLOTS) free_slot = index;
    }
    if (free_slot == ABI_GAP_SLOTS) {
        overflow++;
        return;
    }

    struct abi_gap *gap = &gaps[free_slot];
    if (!gap->count) gap->syscall_number = syscall_number;
    gap->count++;

    struct process *process = process_current();
    gap->last_pid = process ? process->tgid : 0;
    strncpy(gap->last_name, process && process->name[0] ? process->name : "?",
            sizeof(gap->last_name));
    gap->last_name[sizeof(gap->last_name) - 1] = '\0';
}

int abi_gaps_snapshot(unsigned index, struct abi_gap *out) {
    if (!out || index >= ABI_GAP_SLOTS || !gaps[index].count) return -1;
    *out = gaps[index];
    return 0;
}

uint64_t abi_gaps_overflow(void) {
    return overflow;
}

void abi_gaps_clear(void) {
    memset(gaps, 0, sizeof(gaps));
    overflow = 0;
}
