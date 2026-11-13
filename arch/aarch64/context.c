#include <stdint.h>

#include <tunix/process.h>

enum { SLOT_X19, SLOT_X20, SLOT_X29 = 10, SLOT_X30, SLOTS };

uint64_t arch_context_init(uint64_t stack_top, uint64_t reserve, void (*entry)(void),
                           uint64_t first, uint64_t second) {
    uint64_t base = (stack_top - reserve - 96U) & ~15ULL;
    uint64_t *slot = (uint64_t *)base;
    for (unsigned index = 0; index < SLOTS; index++) slot[index] = 0;
    slot[SLOT_X19] = first;
    slot[SLOT_X20] = second;
    slot[SLOT_X30] = (uint64_t)entry;
    return base;
}
