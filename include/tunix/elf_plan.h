#ifndef TUNIX_ELF_PLAN_H
#define TUNIX_ELF_PLAN_H

#include <stddef.h>
#include <stdint.h>

struct tunix_elf_plan {
    uint16_t kind;
    uint16_t phnum;
    uint16_t load_count;
    uint16_t reserved;
    uint64_t load_bias;
    uint64_t entry;
    uint64_t phdr;
    uint64_t image_start;
    uint64_t image_end;
    uint64_t interp_offset;
    uint64_t interp_size;
};

struct tunix_elf_segment {
    uint64_t address;
    uint64_t file_offset;
    uint64_t file_size;
    uint64_t memory_size;
    uint64_t page_start;
    uint64_t page_end;
    uint8_t writable;
    uint8_t executable;
    uint8_t reserved[6];
};

_Static_assert(sizeof(struct tunix_elf_plan) == 64, "tunix_elf_plan must match the Rust layout");
_Static_assert(sizeof(struct tunix_elf_segment) == 56,
               "tunix_elf_segment must match the Rust layout");

int64_t tunix_elf_header_span(const uint8_t *header, size_t length, uint64_t file_size,
                              uint16_t machine);
int tunix_elf_plan(const uint8_t *headers, size_t length, uint64_t file_size, uint16_t machine,
                   uint64_t preferred_base, uint64_t user_limit, struct tunix_elf_plan *out,
                   struct tunix_elf_segment *segments, size_t capacity);
int tunix_elf_interp_valid(const uint8_t *path, size_t length);

#endif
