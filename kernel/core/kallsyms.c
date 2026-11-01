#include <stddef.h>
#include <stdint.h>
#include <tunix/kallsyms.h>
#include <tunix/module.h>

extern const uint64_t kallsyms_count;
extern const uint64_t kallsyms_addresses[];
extern const uint32_t kallsyms_sizes[];
extern const uint32_t kallsyms_offsets[];
extern const char kallsyms_types[];
extern const char kallsyms_names[];
extern const char _stext[];
extern const char _etext[];

uint64_t kallsyms_total(void) { return kallsyms_count; }

int kallsyms_at(uint64_t index, struct kallsyms_symbol *symbol) {
    if (index >= kallsyms_count || !symbol) return -1;
    symbol->name = kallsyms_names + kallsyms_offsets[index];
    symbol->address = kallsyms_addresses[index];
    symbol->size = kallsyms_sizes[index];
    symbol->type = kallsyms_types[index];
    return 0;
}

int kallsyms_is_text(uint64_t address) {
    return address >= (uint64_t)_stext && address < (uint64_t)_etext;
}

int kallsyms_lookup(uint64_t address, struct kallsyms_symbol *symbol) {
    if (!kallsyms_is_text(address) || kallsyms_count == 0) return -1;
    uint64_t low = 0, high = kallsyms_count;
    while (low < high) {
        uint64_t middle = low + (high - low) / 2U;
        if (kallsyms_addresses[middle] <= address) low = middle + 1U;
        else high = middle;
    }
    if (low == 0) return -1;
    uint64_t index = low - 1U;
    if (kallsyms_at(index, symbol) != 0) return -1;
    if (symbol->size && address - symbol->address >= symbol->size) return -1;
    return 0;
}

static void put_char(char *buffer, size_t capacity, size_t *used, char c) {
    if (*used + 1U < capacity) buffer[*used] = c;
    (*used)++;
}

static void put_text(char *buffer, size_t capacity, size_t *used, const char *text) {
    while (*text) put_char(buffer, capacity, used, *text++);
}

static void put_hex(char *buffer, size_t capacity, size_t *used, uint64_t value) {
    static const char digits[] = "0123456789abcdef";
    char reversed[16];
    unsigned count = 0;
    do {
        reversed[count++] = digits[value & 0xFU];
        value >>= 4;
    } while (value);
    put_text(buffer, capacity, used, "0x");
    while (count) put_char(buffer, capacity, used, reversed[--count]);
}

size_t kallsyms_format(uint64_t address, char *buffer, size_t capacity) {
    size_t used = 0;
    struct kallsyms_symbol symbol;
    const char *module = NULL;
    const char *function = NULL;
    uint64_t within = 0, size = 0;
    if (kallsyms_lookup(address, &symbol) == 0) {
        put_text(buffer, capacity, &used, symbol.name);
        put_char(buffer, capacity, &used, '+');
        put_hex(buffer, capacity, &used, address - symbol.address);
        put_char(buffer, capacity, &used, '/');
        put_hex(buffer, capacity, &used, symbol.size);
    } else if (module_symbolize(address, &module, &function, &within, &size) == 0 && function) {
        put_text(buffer, capacity, &used, function);
        put_char(buffer, capacity, &used, '+');
        put_hex(buffer, capacity, &used, within);
        put_char(buffer, capacity, &used, '/');
        put_hex(buffer, capacity, &used, size);
        put_text(buffer, capacity, &used, " [");
        put_text(buffer, capacity, &used, module);
        put_char(buffer, capacity, &used, ']');
    } else if (module_address_owner(address, &module, &within) == 0 && module) {
        put_text(buffer, capacity, &used, module);
        put_char(buffer, capacity, &used, '+');
        put_hex(buffer, capacity, &used, within);
        put_text(buffer, capacity, &used, "/0x0 [");
        put_text(buffer, capacity, &used, module);
        put_char(buffer, capacity, &used, ']');
    } else {
        put_hex(buffer, capacity, &used, address);
    }
    if (capacity) buffer[used < capacity ? used : capacity - 1U] = '\0';
    return used;
}
