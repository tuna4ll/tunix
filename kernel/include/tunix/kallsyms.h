#ifndef TUNIX_KALLSYMS_H
#define TUNIX_KALLSYMS_H

#include <stddef.h>
#include <stdint.h>

struct kallsyms_symbol {
    const char *name;
    uint64_t address;
    uint64_t size;
    char type;
};

uint64_t kallsyms_total(void);
int kallsyms_at(uint64_t index, struct kallsyms_symbol *symbol);
int kallsyms_lookup(uint64_t address, struct kallsyms_symbol *symbol);
int kallsyms_is_text(uint64_t address);
size_t kallsyms_format(uint64_t address, char *buffer, size_t capacity);

#endif
