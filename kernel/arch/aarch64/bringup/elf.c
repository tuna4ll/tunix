#include <stddef.h>
#include <stdint.h>

#include "arch.h"

#define ET_EXEC      2
#define EM_AARCH64   183
#define PT_LOAD      1

#define PF_X 1U
#define PF_W 2U

#define PAGE_SIZE 4096UL
#define PAGE_DOWN(x) ((x) & ~(PAGE_SIZE - 1))
#define PAGE_UP(x)   (((x) + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1))

struct elf64_header {
    uint8_t ident[16];
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

struct elf64_phdr {
    uint32_t type;
    uint32_t flags;
    uint64_t offset;
    uint64_t vaddr;
    uint64_t paddr;
    uint64_t filesz;
    uint64_t memsz;
    uint64_t align;
};

static int header_is_supported(const struct elf64_header *header) {
    if (header->ident[0] != 0x7F || header->ident[1] != 'E' ||
        header->ident[2] != 'L' || header->ident[3] != 'F') return 0;
    if (header->ident[4] != 2 || header->ident[5] != 1) return 0;   // 64-bit LE
    return header->type == ET_EXEC && header->machine == EM_AARCH64;
}

static int load_segment(uint64_t root_pa, const uint8_t *image, uint64_t length,
                        const struct elf64_phdr *segment) {
    unsigned flags = VMM_USER;
    if (segment->flags & PF_W) flags |= VMM_WRITE;
    if (segment->flags & PF_X) flags |= VMM_EXEC;

    uint64_t first = PAGE_DOWN(segment->vaddr);
    uint64_t last = PAGE_UP(segment->vaddr + segment->memsz);
    uint64_t file_end = segment->vaddr + segment->filesz;

    for (uint64_t page = first; page < last; page += PAGE_SIZE) {
        uint64_t frame = (uint64_t)pmm_alloc_page();
        if (!frame) return -1;
        uint8_t *target = (uint8_t *)phys_to_virt(frame);   // arrives zeroed

        uint64_t from = page > segment->vaddr ? page : segment->vaddr;
        uint64_t to = page + PAGE_SIZE < file_end ? page + PAGE_SIZE : file_end;
        if (to > from) {
            uint64_t offset = segment->offset + (from - segment->vaddr);
            if (offset + (to - from) > length) return -1;
            for (uint64_t i = 0; i < to - from; i++)
                target[(from - page) + i] = image[offset + i];
        }

        if (vmm_map(root_pa, page, frame, flags) != 0) return -1;
    }
    return 0;
}

int elf_load_image(uint64_t root_pa, const void *data, uint64_t length,
                   struct elf_image *out) {
    const uint8_t *image = data;
    const struct elf64_header *header = data;

    if (length < sizeof(*header) || !header_is_supported(header)) return -1;
    if (header->phentsize < sizeof(struct elf64_phdr)) return -1;

    uint64_t table = (uint64_t)header->phnum * header->phentsize;
    if (header->phoff + table > length) return -1;

    out->entry = header->entry;
    out->phdr = 0;
    out->phentsize = header->phentsize;
    out->phnum = header->phnum;

    for (unsigned i = 0; i < header->phnum; i++) {
        const struct elf64_phdr *segment =
            (const struct elf64_phdr *)(image + header->phoff +
                                        (uint64_t)i * header->phentsize);
        if (segment->type != PT_LOAD || segment->memsz == 0) continue;
        if (load_segment(root_pa, image, length, segment) != 0) return -1;

        // Where the program headers ended up, which is what AT_PHDR wants.
        if (header->phoff >= segment->offset &&
            header->phoff + table <= segment->offset + segment->filesz)
            out->phdr = segment->vaddr + (header->phoff - segment->offset);
    }

    return 0;
}
