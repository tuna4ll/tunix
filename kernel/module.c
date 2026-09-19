#include <stddef.h>
#include <stdint.h>

#include "include/heap.h"
#include "include/klog.h"
#include "include/kstring.h"
#include "include/module.h"
#include "include/pmm.h"
#include "include/sysfs.h"
#include "include/vmm.h"
#include "include/vmm_arch.h"

extern void kprintf(const char *fmt, ...);

#define ENOENT 2
#define ENOEXEC 8
#define EAGAIN 11
#define ENOMEM 12
#define EBUSY 16
#define EEXIST 17
#define EINVAL 22
#define ENOSPC 28

#define ELFCLASS64 2
#define ELFDATA2LSB 1
#define EV_CURRENT 1
#define ET_REL 1

#if defined(__x86_64__)
#define ELF_MACHINE 62
#elif defined(__aarch64__)
#define ELF_MACHINE 183
#endif

#define SHT_PROGBITS 1
#define SHT_SYMTAB 2
#define SHT_STRTAB 3
#define SHT_RELA 4
#define SHT_NOBITS 8

#define SHF_WRITE 0x1U
#define SHF_ALLOC 0x2U
#define SHF_EXECINSTR 0x4U

#define SHN_UNDEF 0U
#define SHN_ABS 0xFFF1U
#define SHN_COMMON 0xFFF2U

#define STT_FUNC 2

struct elf64_header {
    unsigned char ident[16];
    uint16_t type;
    uint16_t machine;
    uint32_t version;
    uint64_t entry;
    uint64_t phoff;
    uint64_t shoff;
    uint32_t flags;
    uint16_t ehsize;
    uint16_t phentsize;
    uint16_t phnum;
    uint16_t shentsize;
    uint16_t shnum;
    uint16_t shstrndx;
};

struct elf64_section {
    uint32_t name;
    uint32_t type;
    uint64_t flags;
    uint64_t addr;
    uint64_t offset;
    uint64_t size;
    uint32_t link;
    uint32_t info;
    uint64_t addralign;
    uint64_t entsize;
};

struct elf64_symbol {
    uint32_t name;
    uint8_t info;
    uint8_t other;
    uint16_t shndx;
    uint64_t value;
    uint64_t size;
};

struct elf64_rela {
    uint64_t offset;
    uint64_t info;
    int64_t addend;
};

struct image {
    const uint8_t *bytes;
    size_t length;
    const struct elf64_header *header;
    const struct elf64_section *sections;
    unsigned section_count;
    const char *section_names;
    const struct elf64_symbol *symbols;
    unsigned symbol_count;
    const char *strings;
    uint64_t *addresses;
};

static struct module *modules;
static struct module *active;

extern const struct module_export kernel_symbols[];
extern const unsigned kernel_symbol_count;

struct module *module_active(void) { return active; }
unsigned module_kernel_symbol_count(void) { return kernel_symbol_count; }
struct module *module_list(void) { return modules; }

struct module *module_find(const char *name) {
    for (struct module *module = modules; module; module = module->next)
        if (strcmp(module->name, name) == 0) return module;
    return NULL;
}

int module_get(struct module *module) {
    if (!module) return 0;
    if (module->state == MODULE_STATE_UNLOADING) return -EBUSY;
    module->refs++;
    return 0;
}

void module_put(struct module *module) {
    if (module && module->refs) module->refs--;
}

const char *module_state_name(const struct module *module) {
    if (!module) return "Unknown";
    if (module->state == MODULE_STATE_LOADING) return "Loading";
    if (module->state == MODULE_STATE_UNLOADING) return "Unloading";
    return "Live";
}

int module_address_owner(uint64_t address, const char **name, uint64_t *offset) {
    for (struct module *module = modules; module; module = module->next) {
        if (address < module->base || address >= module->base + module->bytes) continue;
        if (name) *name = module->name;
        if (offset) *offset = address - module->base;
        return 0;
    }
    return -1;
}

static uint64_t page_align(uint64_t value) {
    return (value + 0xFFFULL) & ~0xFFFULL;
}

static int range_valid(const struct image *image, uint64_t offset, uint64_t size) {
    return size <= image->length && offset <= image->length - size;
}

static uint64_t reserve_window(uint64_t bytes) {
    uint64_t candidate = MODULE_VIRTUAL_BASE;
    int moved = 1;
    while (moved) {
        moved = 0;
        for (struct module *module = modules; module; module = module->next) {
            if (candidate + bytes > module->base &&
                candidate < module->base + module->bytes) {
                candidate = module->base + module->bytes;
                moved = 1;
            }
        }
    }
    if (candidate + bytes > MODULE_VIRTUAL_BASE + MODULE_VIRTUAL_BYTES) return 0;
    return candidate;
}

static void unmap_window(uint64_t base, uint64_t bytes) {
    vmm_flush_batch_begin();
    for (uint64_t offset = 0; offset < bytes; offset += 4096ULL)
        (void)vmm_unmap_page_in(vmm_kernel_cr3(), base + offset);
    vmm_flush_batch_end();
}

static int map_window(uint64_t base, uint64_t physical, uint64_t bytes) {
    for (uint64_t offset = 0; offset < bytes; offset += 4096ULL) {
        if (vmm_map_page_in(vmm_kernel_cr3(), base + offset, physical + offset,
                            PAGE_WRITE | PAGE_NX) != 0) {
            unmap_window(base, offset);
            return -1;
        }
    }
    return 0;
}

static void protect_range(uint64_t base, uint64_t bytes, uint64_t flags) {
    vmm_flush_batch_begin();
    for (uint64_t offset = 0; offset < bytes; offset += 4096ULL)
        (void)vmm_protect_page_in(vmm_kernel_cr3(), base + offset, flags);
    vmm_flush_batch_end();
}

static const char *modinfo_entry(const struct image *image, const char *key,
                                 unsigned occurrence, char *out, size_t capacity) {
    size_t key_length = strlen(key);
    unsigned seen = 0;
    for (unsigned index = 0; index < image->section_count; index++) {
        const struct elf64_section *section = &image->sections[index];
        if (strcmp(image->section_names + section->name, ".modinfo") != 0) continue;
        if (!range_valid(image, section->offset, section->size)) return NULL;
        const char *text = (const char *)(image->bytes + section->offset);
        uint64_t at = 0;
        while (at < section->size) {
            const char *entry = text + at;
            uint64_t length = 0;
            while (at + length < section->size && entry[length]) length++;
            if (length > key_length && strncmp(entry, key, key_length) == 0 &&
                entry[key_length] == '=' && seen++ == occurrence) {
                const char *value = entry + key_length + 1;
                size_t used = 0;
                while (value[used] && used + 1 < capacity) {
                    out[used] = value[used];
                    used++;
                }
                out[used] = '\0';
                return out;
            }
            at += length + 1;
        }
    }
    return NULL;
}

static const char *modinfo_value(const struct image *image, const char *key,
                                 char *out, size_t capacity) {
    return modinfo_entry(image, key, 0, out, capacity);
}

static int section_index_named(const struct image *image, const char *name) {
    for (unsigned index = 0; index < image->section_count; index++)
        if (strcmp(image->section_names + image->sections[index].name, name) == 0)
            return (int)index;
    return -1;
}

static int lookup_kernel_symbol(const char *name, uint64_t *value) {
    for (unsigned index = 0; index < kernel_symbol_count; index++) {
        if (strcmp(kernel_symbols[index].name, name) == 0) {
            *value = kernel_symbols[index].value;
            return 0;
        }
    }
    return -1;
}

static int note_use(struct module *module, struct module *provider) {
    for (unsigned index = 0; index < module->use_count; index++)
        if (module->uses[index] == provider) return 0;
    if (module->use_count >= MODULE_MAX_USES) return -1;
    if (module_get(provider) != 0) return -1;
    module->uses[module->use_count++] = provider;
    return 0;
}

static int lookup_module_symbol(struct module *module, const char *name,
                                uint64_t *value) {
    for (struct module *provider = modules; provider; provider = provider->next) {
        if (provider == module) continue;
        for (unsigned index = 0; index < provider->export_count; index++) {
            if (strcmp(provider->exports[index].name, name) != 0) continue;
            if (note_use(module, provider) != 0) return -1;
            *value = provider->exports[index].value;
            return 0;
        }
    }
    return -1;
}

static int symbol_address(struct module *module, const struct image *image,
                          uint32_t symbol_index, uint64_t *value) {
    if (symbol_index >= image->symbol_count) return -1;
    const struct elf64_symbol *symbol = &image->symbols[symbol_index];
    const char *name = image->strings + symbol->name;

    if (symbol->shndx == SHN_ABS) {
        *value = symbol->value;
        return 0;
    }
    if (symbol->shndx == SHN_UNDEF) {
        if (!symbol->name) return -1;
        if (lookup_kernel_symbol(name, value) == 0) return 0;
        if (lookup_module_symbol(module, name, value) == 0) return 0;
        kprintf("MODULE: %s needs unknown symbol %s\n", module->name, name);
        return -1;
    }
    if (symbol->shndx >= image->section_count) return -1;
    if (!image->addresses[symbol->shndx]) return -1;
    *value = image->addresses[symbol->shndx] + symbol->value;
    return 0;
}

static void write32(uint64_t place, uint32_t value) {
    memcpy((void *)place, &value, sizeof(value));
}

static void write64(uint64_t place, uint64_t value) {
    memcpy((void *)place, &value, sizeof(value));
}

#if defined(__x86_64__)

static int relocate(uint64_t place, uint64_t symbol, int64_t addend, uint32_t type) {
    uint64_t value = symbol + (uint64_t)addend;
    switch (type) {
    case 0:
        return 0;
    case 1:
        write64(place, value);
        return 0;
    case 24:
        write64(place, value - place);
        return 0;
    case 2:
    case 4: {
        int64_t relative = (int64_t)value - (int64_t)place;
        if (relative < -0x80000000LL || relative > 0x7FFFFFFFLL) return -2;
        write32(place, (uint32_t)(int32_t)relative);
        return 0;
    }
    case 10:
        if (value > 0xFFFFFFFFULL) return -2;
        write32(place, (uint32_t)value);
        return 0;
    case 11: {
        int64_t signed_value = (int64_t)value;
        if (signed_value < -0x80000000LL || signed_value > 0x7FFFFFFFLL) return -2;
        write32(place, (uint32_t)(int32_t)signed_value);
        return 0;
    }
    default:
        return -1;
    }
}

#elif defined(__aarch64__)

static uint32_t read32(uint64_t place) {
    uint32_t value;
    memcpy(&value, (const void *)place, sizeof(value));
    return value;
}

static void insert_bits(uint64_t place, unsigned shift, unsigned width, uint64_t value) {
    uint32_t mask = (uint32_t)(((1ULL << width) - 1ULL) << shift);
    uint32_t instruction = read32(place);
    instruction = (instruction & ~mask) | (uint32_t)((value << shift) & mask);
    write32(place, instruction);
}

static int fits_signed(int64_t value, unsigned bits) {
    int64_t limit = 1LL << (bits - 1);
    return value >= -limit && value < limit;
}

static int relocate_lo12(uint64_t place, uint64_t value, unsigned scale) {
    uint64_t offset = value & 0xFFFULL;
    if (scale && (offset & ((1ULL << scale) - 1ULL))) return -2;
    insert_bits(place, 10, 12, offset >> scale);
    return 0;
}

static int relocate(uint64_t place, uint64_t symbol, int64_t addend, uint32_t type) {
    uint64_t value = symbol + (uint64_t)addend;
    int64_t relative = (int64_t)value - (int64_t)place;
    switch (type) {
    case 0:
        return 0;
    case 257:
        write64(place, value);
        return 0;
    case 258:
        if (value > 0xFFFFFFFFULL) return -2;
        write32(place, (uint32_t)value);
        return 0;
    case 260:
        write64(place, (uint64_t)relative);
        return 0;
    case 261:
        if (!fits_signed(relative, 32)) return -2;
        write32(place, (uint32_t)(int32_t)relative);
        return 0;
    case 263:
    case 264:
        insert_bits(place, 5, 16, value & 0xFFFFULL);
        return 0;
    case 265:
    case 266:
        insert_bits(place, 5, 16, (value >> 16) & 0xFFFFULL);
        return 0;
    case 267:
    case 268:
        insert_bits(place, 5, 16, (value >> 32) & 0xFFFFULL);
        return 0;
    case 269:
        insert_bits(place, 5, 16, (value >> 48) & 0xFFFFULL);
        return 0;
    case 273:
    case 280: {
        if (!fits_signed(relative, 21) || (relative & 3)) return -2;
        insert_bits(place, 5, 19, (uint64_t)(relative >> 2));
        return 0;
    }
    case 279: {
        if (!fits_signed(relative, 16) || (relative & 3)) return -2;
        insert_bits(place, 5, 14, (uint64_t)(relative >> 2));
        return 0;
    }
    case 274: {
        if (!fits_signed(relative, 21)) return -2;
        insert_bits(place, 29, 2, (uint64_t)relative & 3ULL);
        insert_bits(place, 5, 19, (uint64_t)(relative >> 2));
        return 0;
    }
    case 275:
    case 276: {
        int64_t pages = (int64_t)((value & ~0xFFFULL) - (place & ~0xFFFULL));
        if (type == 275 && !fits_signed(pages, 33)) return -2;
        pages >>= 12;
        insert_bits(place, 29, 2, (uint64_t)pages & 3ULL);
        insert_bits(place, 5, 19, (uint64_t)(pages >> 2));
        return 0;
    }
    case 277:
    case 278:
        return relocate_lo12(place, value, 0);
    case 284:
        return relocate_lo12(place, value, 1);
    case 285:
        return relocate_lo12(place, value, 2);
    case 286:
        return relocate_lo12(place, value, 3);
    case 299:
        return relocate_lo12(place, value, 4);
    case 282:
    case 283: {
        if (!fits_signed(relative, 28) || (relative & 3)) return -2;
        insert_bits(place, 0, 26, (uint64_t)(relative >> 2));
        return 0;
    }
    default:
        return -1;
    }
}

#endif

static int apply_relocations(struct module *module, const struct image *image) {
    for (unsigned index = 0; index < image->section_count; index++) {
        const struct elf64_section *section = &image->sections[index];
        if (section->type != SHT_RELA) continue;
        if (section->info >= image->section_count) continue;
        uint64_t target = image->addresses[section->info];
        if (!target) continue;
        if (!range_valid(image, section->offset, section->size)) return -ENOEXEC;

        const struct elf64_rela *entries =
            (const struct elf64_rela *)(image->bytes + section->offset);
        uint64_t count = section->size / sizeof(struct elf64_rela);
        const struct elf64_section *applied = &image->sections[section->info];
        for (uint64_t entry = 0; entry < count; entry++) {
            uint32_t type = (uint32_t)entries[entry].info;
            uint32_t symbol_index = (uint32_t)(entries[entry].info >> 32);
            if (entries[entry].offset > applied->size) return -ENOEXEC;

            uint64_t symbol = 0;
            if (symbol_address(module, image, symbol_index, &symbol) != 0) return -ENOEXEC;

            int result = relocate(target + entries[entry].offset, symbol,
                                  entries[entry].addend, type);
            if (result != 0) {
                kprintf("MODULE: %s relocation %u %s\n", module->name, (unsigned)type,
                        result == -1 ? "is not supported" : "is out of range");
                return -ENOEXEC;
            }
        }
    }
    return 0;
}

static int parse_number(const char *text, uint64_t *out, int *negative) {
    uint64_t value = 0;
    unsigned base = 10;
    *negative = 0;
    if (*text == '-') { *negative = 1; text++; }
    if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) { base = 16; text += 2; }
    if (!*text) return -1;
    while (*text) {
        unsigned digit;
        if (*text >= '0' && *text <= '9') digit = (unsigned)(*text - '0');
        else if (base == 16 && *text >= 'a' && *text <= 'f') digit = (unsigned)(*text - 'a') + 10U;
        else if (base == 16 && *text >= 'A' && *text <= 'F') digit = (unsigned)(*text - 'A') + 10U;
        else return -1;
        if (digit >= base) return -1;
        value = value * base + digit;
        text++;
    }
    *out = value;
    return 0;
}

static int assign_value(const struct module_param *param, char *value) {
    uint64_t number = 0;
    int negative = 0;
    switch (param->type) {
    case MODULE_PARAM_INT:
        if (parse_number(value, &number, &negative) != 0) return -EINVAL;
        *(int *)param->value = negative ? -(int)number : (int)number;
        return 0;
    case MODULE_PARAM_UINT:
        if (parse_number(value, &number, &negative) != 0 || negative) return -EINVAL;
        *(unsigned *)param->value = (unsigned)number;
        return 0;
    case MODULE_PARAM_BOOL:
        if (strcmp(value, "1") == 0 || strcmp(value, "y") == 0 ||
            strcmp(value, "Y") == 0 || strcmp(value, "on") == 0)
            *(int *)param->value = 1;
        else if (strcmp(value, "0") == 0 || strcmp(value, "n") == 0 ||
                 strcmp(value, "N") == 0 || strcmp(value, "off") == 0)
            *(int *)param->value = 0;
        else return -EINVAL;
        return 0;
    case MODULE_PARAM_STRING:
        *(char **)param->value = value;
        return 0;
    default:
        return -EINVAL;
    }
}

static int assign_parameter(struct module *module, const char *name, char *value) {
    for (unsigned index = 0; index < module->param_count; index++) {
        const struct module_param *param = &module->params[index];
        if (strcmp(param->name, name) == 0) return assign_value(param, value);
    }
    kprintf("MODULE: %s has no parameter %s\n", module->name, name);
    return -EINVAL;
}

int module_param_set(struct module *module, unsigned index, const char *text,
                     size_t length) {
    if (!module || index >= module->param_count) return -EINVAL;
    const struct module_param *param = &module->params[index];
    /* A string would have to outlive the write, and nothing owns it. */
    if (param->type == MODULE_PARAM_STRING) return -EINVAL;

    char value[32];
    size_t used = 0;
    while (used < length && used + 1 < sizeof(value) && text[used] != '\n' &&
           text[used] != '\0') {
        value[used] = text[used];
        used++;
    }
    value[used] = '\0';
    if (!used) return -EINVAL;
    return assign_value(param, value);
}

static int apply_parameters(struct module *module) {
    char *text = module->arguments;
    if (!text) return 0;
    while (*text) {
        while (*text == ' ' || *text == '\t' || *text == '\n') text++;
        if (!*text) break;
        char *name = text;
        while (*text && *text != '=' && *text != ' ' && *text != '\t') text++;
        if (*text != '=') return -EINVAL;
        *text++ = '\0';
        char *value = text;
        while (*text && *text != ' ' && *text != '\t' && *text != '\n') text++;
        if (*text) *text++ = '\0';
        int result = assign_parameter(module, name, value);
        if (result != 0) return result;
    }
    return 0;
}

int module_param_format(const struct module *module, unsigned index, char *out,
                        size_t capacity) {
    if (!module || index >= module->param_count || capacity < 24) return -1;
    const struct module_param *param = &module->params[index];
    if (param->type == MODULE_PARAM_STRING) {
        const char *value = *(const char *const *)param->value;
        size_t used = 0;
        if (value)
            while (value[used] && used + 2 < capacity) { out[used] = value[used]; used++; }
        out[used++] = '\n';
        out[used] = '\0';
        return (int)used;
    }
    int64_t value = param->type == MODULE_PARAM_UINT
        ? (int64_t)*(const unsigned *)param->value
        : (int64_t)*(const int *)param->value;
    char digits[24];
    size_t count = 0;
    int negative = value < 0;
    uint64_t magnitude = negative ? (uint64_t)(-value) : (uint64_t)value;
    do {
        digits[count++] = (char)('0' + magnitude % 10ULL);
        magnitude /= 10ULL;
    } while (magnitude);
    size_t used = 0;
    if (negative) out[used++] = '-';
    while (count) out[used++] = digits[--count];
    out[used++] = '\n';
    out[used] = '\0';
    return (int)used;
}

static void release_module(struct module *module) {
    for (unsigned index = 0; index < module->use_count; index++)
        module_put(module->uses[index]);
    if (module->base) unmap_window(module->base, module->bytes);
    if (module->physical)
        pmm_free_pages((void *)module->physical, module->bytes / 4096ULL);
    kfree(module->arguments);
    kfree(module);
}

static int place_sections(struct module *module, struct image *image) {
    uint64_t text = 0, rodata = 0, data = 0, bss = 0;

    for (unsigned pass = 0; pass < 4U; pass++) {
        for (unsigned index = 0; index < image->section_count; index++) {
            const struct elf64_section *section = &image->sections[index];
            if (!(section->flags & SHF_ALLOC) || !section->size) continue;
            unsigned kind;
            if (section->flags & SHF_EXECINSTR) kind = 0;
            else if (!(section->flags & SHF_WRITE)) kind = 1;
            else if (section->type == SHT_NOBITS) kind = 3;
            else kind = 2;
            if (kind != pass) continue;

            uint64_t *cursor = pass == 0 ? &text : pass == 1 ? &rodata
                             : pass == 2 ? &data : &bss;
            uint64_t alignment = section->addralign ? section->addralign : 1ULL;
            if (alignment > 4096ULL) alignment = 4096ULL;
            *cursor = (*cursor + alignment - 1ULL) & ~(alignment - 1ULL);
            image->addresses[index] = *cursor;
            *cursor += section->size;
        }
        if (pass == 0) { rodata = page_align(text); }
        else if (pass == 1) { data = page_align(rodata); }
        else if (pass == 2) { bss = data; }
    }

    uint64_t total = page_align(bss);
    if (!total || total > MODULE_VIRTUAL_BYTES) return -ENOMEM;

    uint64_t base = reserve_window(total);
    if (!base) return -ENOSPC;
    uint64_t physical = (uint64_t)pmm_alloc_pages(total / 4096ULL, 4096ULL);
    if (!physical) return -ENOMEM;
    if (map_window(base, physical, total) != 0) {
        pmm_free_pages((void *)physical, total / 4096ULL);
        return -ENOMEM;
    }

    module->base = base;
    module->physical = physical;
    module->bytes = total;
    module->text = base;
    module->text_bytes = page_align(text);
    module->rodata = base + page_align(text);
    module->data = base + page_align(rodata);

    memset((void *)base, 0, total);
    for (unsigned index = 0; index < image->section_count; index++) {
        const struct elf64_section *section = &image->sections[index];
        if (!(section->flags & SHF_ALLOC) || !section->size) continue;
        image->addresses[index] += base;
        if (section->type == SHT_NOBITS) continue;
        if (!range_valid(image, section->offset, section->size)) return -ENOEXEC;
        memcpy((void *)image->addresses[index], image->bytes + section->offset,
               section->size);
    }
    return 0;
}

static int protect_module(struct module *module) {
    protect_range(module->text, module->text_bytes, PAGE_PRESENT);
    protect_range(module->rodata, module->data - module->rodata,
                  PAGE_PRESENT | PAGE_NX);
    protect_range(module->data, module->base + module->bytes - module->data,
                  PAGE_PRESENT | PAGE_WRITE | PAGE_NX);
    for (uint64_t offset = 0; offset < module->text_bytes; offset += 4096ULL)
        vmm_arch_sync_executable(module->physical + offset);

    for (uint64_t offset = 0; offset < module->text_bytes; offset += 4096ULL) {
        uint64_t flags = 0;
        if (vmm_translate(vmm_kernel_cr3(), module->text + offset, NULL, &flags) != 0 ||
            (flags & PAGE_NX)) {
            kprintf("MODULE: %s text at %p did not become executable\n",
                    module->name, (void *)(module->text + offset));
            return -ENOEXEC;
        }
    }
    return 0;
}

static int read_tables(struct module *module, const struct image *image) {
    int index = section_index_named(image, ".tunix_ksym");
    if (index >= 0 && image->addresses[index]) {
        if (image->sections[index].size % sizeof(struct module_export)) return -ENOEXEC;
        module->exports = (const struct module_export *)image->addresses[index];
        module->export_count =
            (unsigned)(image->sections[index].size / sizeof(struct module_export));
    }
    index = section_index_named(image, ".tunix_param");
    if (index >= 0 && image->addresses[index]) {
        if (image->sections[index].size % sizeof(struct module_param)) return -ENOEXEC;
        module->params = (const struct module_param *)image->addresses[index];
        module->param_count =
            (unsigned)(image->sections[index].size / sizeof(struct module_param));
    }
    index = section_index_named(image, ".tunix_module");
    if (index < 0 || !image->addresses[index] ||
        image->sections[index].size < sizeof(struct module_descriptor))
        return -ENOEXEC;
    return index;
}

static int prepare_image(struct image *image, const void *contents, size_t bytes) {
    memset(image, 0, sizeof(*image));
    image->bytes = (const uint8_t *)contents;
    image->length = bytes;

    if (bytes < sizeof(struct elf64_header)) return -ENOEXEC;
    const struct elf64_header *header = (const struct elf64_header *)contents;
    if (header->ident[0] != 0x7F || header->ident[1] != 'E' || header->ident[2] != 'L' ||
        header->ident[3] != 'F' || header->ident[4] != ELFCLASS64 ||
        header->ident[5] != ELFDATA2LSB || header->type != ET_REL ||
        header->machine != ELF_MACHINE || header->version != EV_CURRENT)
        return -ENOEXEC;
    if (header->shentsize != sizeof(struct elf64_section) || !header->shnum ||
        header->shstrndx >= header->shnum)
        return -ENOEXEC;

    image->header = header;
    image->section_count = header->shnum;
    if (!range_valid(image, header->shoff,
                     (uint64_t)header->shnum * sizeof(struct elf64_section)))
        return -ENOEXEC;
    image->sections = (const struct elf64_section *)(image->bytes + header->shoff);

    const struct elf64_section *names = &image->sections[header->shstrndx];
    if (!range_valid(image, names->offset, names->size)) return -ENOEXEC;
    image->section_names = (const char *)(image->bytes + names->offset);

    for (unsigned index = 0; index < image->section_count; index++) {
        const struct elf64_section *section = &image->sections[index];
        if (section->type != SHT_SYMTAB) continue;
        if (section->link >= image->section_count) return -ENOEXEC;
        const struct elf64_section *strings = &image->sections[section->link];
        if (!range_valid(image, section->offset, section->size) ||
            !range_valid(image, strings->offset, strings->size))
            return -ENOEXEC;
        image->symbols = (const struct elf64_symbol *)(image->bytes + section->offset);
        image->symbol_count = (unsigned)(section->size / sizeof(struct elf64_symbol));
        image->strings = (const char *)(image->bytes + strings->offset);
        break;
    }
    return image->symbols ? 0 : -ENOEXEC;
}

int module_image_info(const void *contents, size_t bytes, const char *key,
                      unsigned occurrence, char *out, size_t capacity) {
    struct image image;
    if (!out || !capacity) return -EINVAL;
    out[0] = '\0';
    int status = prepare_image(&image, contents, bytes);
    if (status != 0) return status;
    return modinfo_entry(&image, key, occurrence, out, capacity) ? 0 : -ENOENT;
}

int module_export_value(const struct module *module, const char *name,
                        uint64_t *value) {
    if (!module || !name || !value) return -EINVAL;
    for (unsigned index = 0; index < module->export_count; index++) {
        if (strcmp(module->exports[index].name, name) != 0) continue;
        *value = module->exports[index].value;
        return 0;
    }
    return -ENOENT;
}

int module_load(const void *contents, size_t bytes, const char *arguments) {
    struct image image;
    int status = prepare_image(&image, contents, bytes);
    if (status != 0) return status;

    char name[MODULE_NAME_MAX];
    char vermagic[64] = { 0 };
    if (!modinfo_value(&image, "name", name, sizeof(name))) return -ENOEXEC;
    if (!modinfo_value(&image, "vermagic", vermagic, sizeof(vermagic)) ||
        strcmp(vermagic, MODULE_VERMAGIC) != 0) {
        kprintf("MODULE: %s was built for \"%s\", this kernel is \"%s\"\n", name,
                vermagic, MODULE_VERMAGIC);
        return -ENOEXEC;
    }
    if (module_find(name)) return -EEXIST;

    struct module *module = kmalloc(sizeof(*module));
    if (!module) return -ENOMEM;
    memset(module, 0, sizeof(*module));
    strncpy(module->name, name, sizeof(module->name) - 1);
    module->state = MODULE_STATE_LOADING;

    image.addresses = kmalloc(sizeof(uint64_t) * image.section_count);
    if (!image.addresses) {
        kfree(module);
        return -ENOMEM;
    }
    memset(image.addresses, 0, sizeof(uint64_t) * image.section_count);

    if (arguments && arguments[0]) {
        size_t length = strlen(arguments);
        module->arguments = kmalloc(length + 1);
        if (!module->arguments) {
            kfree(image.addresses);
            kfree(module);
            return -ENOMEM;
        }
        memcpy(module->arguments, arguments, length + 1);
    }

    int result = place_sections(module, &image);
    if (result != 0) goto failed;

    result = apply_relocations(module, &image);
    if (result != 0) goto failed;

    int descriptor_index = read_tables(module, &image);
    if (descriptor_index < 0) {
        result = descriptor_index;
        goto failed;
    }
    const struct module_descriptor *descriptor =
        (const struct module_descriptor *)image.addresses[descriptor_index];

    result = apply_parameters(module);
    if (result != 0) goto failed;

    result = protect_module(module);
    if (result != 0) goto failed;

    module->exit = descriptor->exit;
    module->next = modules;
    modules = module;

    struct module *previous = active;
    active = module;
    result = descriptor->init ? descriptor->init() : 0;
    active = previous;

    if (result != 0) {
        modules = module->next;
        goto failed;
    }

    module->state = MODULE_STATE_LIVE;
    kfree(image.addresses);
    sysfs_module_added(module);
    kprintf("MODULE: %s loaded at %p, %u bytes\n", module->name,
            (void *)module->base, (unsigned)module->bytes);
    return 0;

failed:
    kfree(image.addresses);
    release_module(module);
    return result;
}

int module_unload(const char *name, unsigned flags) {
    (void)flags;
    struct module *module = module_find(name);
    if (!module) return -ENOENT;
    if (module->state != MODULE_STATE_LIVE) return -EBUSY;
    if (module->refs) return -EBUSY;

    module->state = MODULE_STATE_UNLOADING;
    if (module->exit) {
        struct module *previous = active;
        active = module;
        module->exit();
        active = previous;
    }

    struct module **link = &modules;
    while (*link && *link != module) link = &(*link)->next;
    if (*link) *link = module->next;

    sysfs_module_removed(module->name);
    kprintf("MODULE: %s unloaded\n", module->name);
    release_module(module);
    return 0;
}
