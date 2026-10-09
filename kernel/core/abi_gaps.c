#include <stddef.h>
#include <stdint.h>

#include <tunix/abi_gaps.h>
#include <tunix/kstring.h>
#include <tunix/lock.h>
#include <tunix/process.h>

static struct abi_gap gaps[ABI_GAP_SLOTS];
static uint64_t overflow;
static struct lock gaps_lock = LOCK_INITIALIZER("abi gaps", LOCK_RANK_LEAF);

void abi_gaps_note(uint64_t syscall_number) {
    struct process *process = process_current();
    lock_acquire(&gaps_lock);
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
        lock_release(&gaps_lock);
        return;
    }

    struct abi_gap *gap = &gaps[free_slot];
    if (!gap->count) gap->syscall_number = syscall_number;
    gap->count++;
    gap->last_pid = process ? process->tgid : 0;
    strncpy(gap->last_name, process && process->name[0] ? process->name : "?",
            sizeof(gap->last_name));
    gap->last_name[sizeof(gap->last_name) - 1] = '\0';
    lock_release(&gaps_lock);
}

int abi_gaps_snapshot(unsigned index, struct abi_gap *out) {
    if (!out || index >= ABI_GAP_SLOTS) return -1;
    lock_acquire(&gaps_lock);
    int found = gaps[index].count != 0;
    if (found) *out = gaps[index];
    lock_release(&gaps_lock);
    return found ? 0 : -1;
}

uint64_t abi_gaps_overflow(void) { return overflow; }

void abi_gaps_clear(void) {
    lock_acquire(&gaps_lock);
    memset(gaps, 0, sizeof(gaps));
    overflow = 0;
    lock_release(&gaps_lock);
}
