#include <stdint.h>

#include "../../include/process.h"

enum { SLOT_R15, SLOT_R14, SLOT_R13, SLOT_R12, SLOT_RBX, SLOT_RBP, SLOT_RETURN, SLOTS };

uint64_t arch_context_init(uint64_t stack_top, uint64_t reserve, void (*entry)(void),
                           uint64_t first, uint64_t second) {
    uint64_t base = (stack_top - reserve - 64U) & ~15ULL;
    uint64_t *slot = (uint64_t *)base;
    for (unsigned index = 0; index < SLOTS; index++) slot[index] = 0;
    slot[SLOT_R12] = first;
    slot[SLOT_R13] = second;
    slot[SLOT_RETURN] = (uint64_t)entry;
    return base;
}
