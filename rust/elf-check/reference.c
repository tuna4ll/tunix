#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define ELFCLASS64      2
#define ELFDATA2LSB     1
#define EV_CURRENT      1
#define ET_EXEC         2
#define ET_DYN          3
#define PT_LOAD         1
#define PT_INTERP       3
#define PF_X            1
#define PF_W            2
#define MAX_INTERP_PATH 4096

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
} __attribute__((packed));

struct elf64_program_header {
    uint32_t type;
    uint32_t flags;
    uint64_t offset;
    uint64_t vaddr;
    uint64_t paddr;
    uint64_t filesz;
    uint64_t memsz;
    uint64_t align;
} __attribute__((packed));

struct reference_plan {
    uint16_t kind;
    uint16_t phnum;
    uint16_t load_count;
    uint16_t entry_wrapped;
    uint64_t load_bias;
    uint64_t entry;
    uint64_t phdr;
    uint64_t image_start;
    uint64_t image_end;
    uint64_t interp_offset;
    uint64_t interp_size;
};

struct reference_segment {
    uint64_t page_start;
    uint64_t page_end;
    uint64_t address;
    uint32_t writable;
    uint32_t executable;
};

static uint64_t align_down(uint64_t value, uint64_t alignment) { return value & ~(alignment - 1); }

static uint64_t align_up(uint64_t value, uint64_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

static int power_of_two(uint64_t value) { return value && !(value & (value - 1)); }

static int add_overflows(uint64_t left, uint64_t right) { return left > UINT64_MAX - right; }

static int valid_header(const struct elf64_header *header, uint64_t file_size, uint16_t machine) {
    if (file_size < sizeof(*header)) return 0;
    if (header->ident[0] != 0x7F || header->ident[1] != 'E' || header->ident[2] != 'L' ||
        header->ident[3] != 'F')
        return 0;
    if (header->ident[4] != ELFCLASS64 || header->ident[5] != ELFDATA2LSB ||
        header->ident[6] != EV_CURRENT)
        return 0;
    if ((header->type != ET_EXEC && header->type != ET_DYN) || header->machine != machine ||
        header->version != EV_CURRENT)
        return 0;
    if (header->ehsize != sizeof(*header) ||
        header->phentsize != sizeof(struct elf64_program_header) || header->phnum == 0)
        return 0;
    if (header->phoff > file_size) return 0;
    uint64_t table_size = (uint64_t)header->phnum * header->phentsize;
    if (table_size > file_size - header->phoff) return 0;
    return 1;
}

static int validate_programs(const struct elf64_header *header,
                             const struct elf64_program_header *programs, uint64_t file_size,
                             uint64_t *minimum_out, uint64_t *maximum_out) {
    uint64_t minimum = UINT64_MAX;
    uint64_t maximum = 0;
    int load_count = 0;

    for (uint16_t i = 0; i < header->phnum; i++) {
        const struct elf64_program_header *program = &programs[i];
        if (program->type == PT_INTERP) {
            if (program->filesz < 2 || program->filesz > MAX_INTERP_PATH ||
                program->offset > file_size || program->filesz > file_size - program->offset)
                return -1;
            continue;
        }
        if (program->type != PT_LOAD) continue;
        load_count++;
        if (program->memsz < program->filesz) return -1;
        if (program->filesz &&
            (program->offset > file_size || program->filesz > file_size - program->offset))
            return -1;
        if (program->align > 1 && !power_of_two(program->align)) return -1;
        if ((program->vaddr & 0xFFFULL) != (program->offset & 0xFFFULL)) return -1;
        if (add_overflows(program->vaddr, program->memsz)) return -1;
        uint64_t start = align_down(program->vaddr, 4096);
        uint64_t end = align_up(program->vaddr + program->memsz, 4096);
        if (start < minimum) minimum = start;
        if (end > maximum) maximum = end;
    }

    if (!load_count || minimum == UINT64_MAX || maximum <= minimum) return -1;
    *minimum_out = minimum;
    *maximum_out = maximum;
    return 0;
}

static int choose_load_bias(const struct elf64_header *header, uint64_t minimum,
                            uint64_t preferred_base, uint64_t *bias_out) {
    if (header->type == ET_EXEC) {
        *bias_out = 0;
        return minimum < 0x10000ULL ? -1 : 0;
    }
    if (preferred_base < minimum) return -1;
    *bias_out = preferred_base - minimum;
    return 0;
}

static int map_segment_pages(const struct elf64_program_header *program, uint64_t load_bias,
                             uint64_t user_limit) {
    if (add_overflows(program->vaddr, load_bias) ||
        add_overflows(program->vaddr + load_bias, program->memsz))
        return -1;
    uint64_t virtual_address = program->vaddr + load_bias;
    if (virtual_address < 0x10000ULL || virtual_address >= user_limit ||
        program->memsz > user_limit - virtual_address)
        return -1;
    return 0;
}

static uint64_t program_header_virtual(const struct elf64_header *header,
                                       const struct elf64_program_header *programs,
                                       uint64_t load_bias) {
    uint64_t table_size = (uint64_t)header->phnum * header->phentsize;
    for (uint16_t i = 0; i < header->phnum; i++) {
        const struct elf64_program_header *program = &programs[i];
        if (program->type != PT_LOAD) continue;
        if (header->phoff < program->offset) continue;
        uint64_t relative = header->phoff - program->offset;
        if (relative <= program->filesz && table_size <= program->filesz - relative) {
            return load_bias + program->vaddr + relative;
        }
    }
    return 0;
}

static int page_mapped(const struct elf64_header *header,
                       const struct elf64_program_header *programs, uint64_t load_bias,
                       uint64_t address) {
    for (uint16_t i = 0; i < header->phnum; i++) {
        const struct elf64_program_header *program = &programs[i];
        if (program->type != PT_LOAD) continue;
        uint64_t virtual_address = program->vaddr + load_bias;
        uint64_t start = align_down(virtual_address, 4096);
        uint64_t end = align_up(virtual_address + program->memsz, 4096);
        if (address >= start && address < end) return 1;
    }
    return 0;
}

int reference_header_span(const uint8_t *bytes, size_t length, uint64_t file_size, uint16_t machine,
                          uint64_t *span_out) {
    struct elf64_header probe;
    if (length < sizeof(probe) || file_size < sizeof(probe)) return -1;
    memcpy(&probe, bytes, sizeof(probe));
    if (!valid_header(&probe, file_size, machine)) return -1;
    uint64_t span = probe.phoff + (uint64_t)probe.phnum * probe.phentsize;
    if (span > file_size || span > 64ULL * 1024ULL) return -1;
    *span_out = span;
    return 0;
}

int reference_plan(const uint8_t *image, size_t length, uint64_t file_size, uint16_t machine,
                   uint64_t preferred_base, uint64_t user_limit, struct reference_plan *plan,
                   struct reference_segment *segments, size_t capacity) {
    uint64_t span;
    if (reference_header_span(image, length, file_size, machine, &span) != 0 || length < span)
        return -1;
    const struct elf64_header *header = (const struct elf64_header *)image;
    const struct elf64_program_header *programs =
        (const struct elf64_program_header *)(image + header->phoff);

    uint64_t minimum, maximum, load_bias;
    if (validate_programs(header, programs, file_size, &minimum, &maximum) != 0 ||
        choose_load_bias(header, minimum, preferred_base, &load_bias) != 0)
        return -1;
    if (add_overflows(maximum, load_bias) || maximum + load_bias > user_limit) return -1;

    size_t count = 0;
    for (uint16_t i = 0; i < header->phnum; i++) {
        if (programs[i].type != PT_LOAD) continue;
        if (map_segment_pages(&programs[i], load_bias, user_limit) != 0) return -1;
        uint64_t virtual_address = programs[i].vaddr + load_bias;
        if (count < capacity) {
            segments[count].address = virtual_address;
            segments[count].page_start = align_down(virtual_address, 4096);
            segments[count].page_end = align_up(virtual_address + programs[i].memsz, 4096);
            segments[count].writable = (programs[i].flags & PF_W) != 0;
            segments[count].executable = (programs[i].flags & PF_X) != 0;
        }
        count++;
    }

    uint64_t entry = header->entry + load_bias;
    if (entry < 0x10000ULL || entry >= user_limit ||
        !page_mapped(header, programs, load_bias, entry))
        return -1;

    const struct elf64_program_header *interp = NULL;
    for (uint16_t i = 0; i < header->phnum; i++) {
        if (programs[i].type != PT_INTERP) continue;
        if (interp) return -1;
        interp = &programs[i];
    }
    uint64_t phdr = program_header_virtual(header, programs, load_bias);
    if (interp && !phdr) return -1;

    memset(plan, 0, sizeof(*plan));
    plan->kind = header->type;
    plan->phnum = header->phnum;
    plan->load_count = (uint16_t)count;
    plan->entry_wrapped = add_overflows(header->entry, load_bias);
    plan->load_bias = load_bias;
    plan->entry = entry;
    plan->phdr = phdr;
    plan->image_start = minimum + load_bias;
    plan->image_end = maximum + load_bias;
    plan->interp_offset = interp ? interp->offset : 0;
    plan->interp_size = interp ? interp->filesz : 0;
    return 0;
}

int reference_interp_valid(const char *path, size_t length) {
    if (length < 2 || length > MAX_INTERP_PATH) return 0;
    if (path[length - 1] != '\0') return 0;
    if (path[0] != '/' || strlen(path) + 1 != length) return 0;
    return 1;
}
