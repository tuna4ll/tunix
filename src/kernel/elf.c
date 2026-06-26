#include <stddef.h>
#include <stdint.h>
#include "include/elf.h"
#include "include/kstring.h"
#include "include/pmm.h"
#include "include/process.h"
#include "include/vfs.h"
#include "include/vmm.h"

#define ELFCLASS64 2
#define ELFDATA2LSB 1
#define ET_EXEC 2
#define EM_X86_64 62
#define PT_LOAD 1
#define PF_W 2
#define USER_STACK_TOP 0x00007FFFFFF00000ULL
#define USER_STACK_PAGES 32ULL
#define MAX_ARGC 64
#define MAX_ENVC 64

#define AT_NULL 0
#define AT_PHDR 3
#define AT_PHENT 4
#define AT_PHNUM 5
#define AT_PAGESZ 6
#define AT_BASE 7
#define AT_FLAGS 8
#define AT_ENTRY 9
#define AT_UID 11
#define AT_EUID 12
#define AT_GID 13
#define AT_EGID 14
#define AT_PLATFORM 15
#define AT_HWCAP 16
#define AT_CLKTCK 17
#define AT_SECURE 23
#define AT_RANDOM 25
#define AT_HWCAP2 26
#define AT_EXECFN 31

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

static uint64_t align_up(uint64_t value, uint64_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

static int valid_header(const struct elf64_header *header, uint64_t file_size) {
    if (file_size < sizeof(*header)) return 0;
    if (header->ident[0] != 0x7F || header->ident[1] != 'E' ||
        header->ident[2] != 'L' || header->ident[3] != 'F') return 0;
    if (header->ident[4] != ELFCLASS64 || header->ident[5] != ELFDATA2LSB) return 0;
    if (header->type != ET_EXEC || header->machine != EM_X86_64) return 0;
    if (header->phentsize != sizeof(struct elf64_program_header)) return 0;
    if (header->phoff > file_size) return 0;
    uint64_t table_size = (uint64_t)header->phnum * header->phentsize;
    if (table_size > file_size - header->phoff) return 0;
    if (header->entry < 0x10000ULL || header->entry >= USER_ADDRESS_LIMIT) return 0;
    return 1;
}

static int map_segment_pages(struct process *process,
                             const struct elf64_program_header *program) {
    if (program->memsz < program->filesz) return -1;
    if (program->vaddr < 0x10000ULL || program->vaddr >= USER_ADDRESS_LIMIT) return -1;
    if (program->memsz > USER_ADDRESS_LIMIT - program->vaddr) return -1;

    uint64_t start = program->vaddr & ~0xFFFULL;
    uint64_t end = align_up(program->vaddr + program->memsz, 4096);
    uint64_t flags = PAGE_USER | PAGE_PRESENT;
    if (program->flags & PF_W) flags |= PAGE_WRITE;

    for (uint64_t address = start; address < end; address += 4096) {
        if (vmm_translate(process->cr3, address, NULL, NULL) == 0) continue;
        uint64_t physical = (uint64_t)pmm_alloc_page();
        memset(vmm_phys_to_virt(physical), 0, 4096);
        if (vmm_map_page_in(process->cr3, address, physical, flags) != 0) return -1;
    }
    return 0;
}

static int copy_segment(struct process *process, const uint8_t *image,
                        const struct elf64_program_header *program) {
    uint64_t remaining = program->filesz;
    uint64_t source_offset = program->offset;
    uint64_t destination = program->vaddr;
    while (remaining) {
        uint64_t physical;
        if (vmm_translate(process->cr3, destination, &physical, NULL) != 0) return -1;
        uint64_t chunk = 4096 - (destination & 0xFFFULL);
        if (chunk > remaining) chunk = remaining;
        memcpy(vmm_phys_to_virt(physical), image + source_offset, (size_t)chunk);
        destination += chunk;
        source_offset += chunk;
        remaining -= chunk;
    }
    return 0;
}

static int push_bytes(struct process *process, uint64_t *sp,
                      const void *data, size_t size) {
    if (*sp < size) return -1;
    *sp -= size;
    return vmm_copy_to_space(process->cr3, *sp, data, size);
}

static int push_u64(struct process *process, uint64_t *sp, uint64_t value) {
    return push_bytes(process, sp, &value, sizeof(value));
}

static uint64_t program_header_virtual(const struct elf64_header *header,
                                       const struct elf64_program_header *programs) {
    uint64_t table_size = (uint64_t)header->phnum * header->phentsize;
    for (uint16_t i = 0; i < header->phnum; i++) {
        const struct elf64_program_header *program = &programs[i];
        if (program->type != PT_LOAD) continue;
        if (header->phoff < program->offset) continue;
        uint64_t relative = header->phoff - program->offset;
        if (relative <= program->filesz && table_size <= program->filesz - relative) {
            return program->vaddr + relative;
        }
    }
    return 0;
}

static int build_initial_stack(struct process *process,
                               const struct elf64_header *header,
                               const struct elf64_program_header *programs,
                               const char *const argv[], const char *const envp[]) {
    uint64_t argv_addresses[MAX_ARGC];
    uint64_t env_addresses[MAX_ENVC];
    size_t argc = 0, envc = 0;
    while (argv && argv[argc]) {
        if (argc >= MAX_ARGC) return -1;
        argc++;
    }
    while (envp && envp[envc]) {
        if (envc >= MAX_ENVC) return -1;
        envc++;
    }

    uint64_t sp = USER_STACK_TOP;
    static const char platform[] = "x86_64";
    const uint8_t random_bytes[16] = {
        0x54, 0x55, 0x4e, 0x49, 0x58, 0x12, 0x34, 0x56,
        0x9a, 0xbc, 0xde, 0xf0, 0x6d, 0x75, 0x73, 0x6c
    };
    if (push_bytes(process, &sp, random_bytes, sizeof(random_bytes)) != 0) return -1;
    uint64_t random_address = sp;
    if (push_bytes(process, &sp, platform, sizeof(platform)) != 0) return -1;
    uint64_t platform_address = sp;

    for (size_t i = envc; i > 0; i--) {
        size_t length = strlen(envp[i - 1]) + 1;
        if (push_bytes(process, &sp, envp[i - 1], length) != 0) return -1;
        env_addresses[i - 1] = sp;
    }
    for (size_t i = argc; i > 0; i--) {
        size_t length = strlen(argv[i - 1]) + 1;
        if (push_bytes(process, &sp, argv[i - 1], length) != 0) return -1;
        argv_addresses[i - 1] = sp;
    }

    sp &= ~15ULL;
    uint64_t phdr = program_header_virtual(header, programs);
    uint64_t execfn = argc ? argv_addresses[0] : 0;
    const uint64_t auxv[][2] = {
        {AT_NULL, 0},
        {AT_EXECFN, execfn},
        {AT_HWCAP2, 0},
        {AT_RANDOM, random_address},
        {AT_SECURE, 0},
        {AT_CLKTCK, 100},
        {AT_HWCAP, 0},
        {AT_PLATFORM, platform_address},
        {AT_EGID, 0}, {AT_GID, 0}, {AT_EUID, 0}, {AT_UID, 0},
        {AT_ENTRY, process->entry},
        {AT_FLAGS, 0},
        {AT_BASE, 0},
        {AT_PHNUM, header->phnum},
        {AT_PHENT, header->phentsize},
        {AT_PHDR, phdr},
        {AT_PAGESZ, 4096}
    };
    const size_t aux_count = sizeof(auxv) / sizeof(auxv[0]);
    size_t stack_words = aux_count * 2 + 1 + envc + 1 + argc + 1;
    if ((sp - stack_words * sizeof(uint64_t)) & 15ULL) {
        if (push_u64(process, &sp, 0) != 0) return -1;
    }
    for (size_t i = 0; i < aux_count; i++) {
        if (push_u64(process, &sp, auxv[i][1]) != 0 ||
            push_u64(process, &sp, auxv[i][0]) != 0) return -1;
    }

    if (push_u64(process, &sp, 0) != 0) return -1;
    for (size_t i = envc; i > 0; i--) if (push_u64(process, &sp, env_addresses[i - 1]) != 0) return -1;
    if (push_u64(process, &sp, 0) != 0) return -1;
    for (size_t i = argc; i > 0; i--) if (push_u64(process, &sp, argv_addresses[i - 1]) != 0) return -1;
    if (push_u64(process, &sp, argc) != 0) return -1;

    if (sp & 15ULL) return -1;
    process->user_stack_top = sp;
    return 0;
}

int elf_load_process(struct process *process, struct vfs_node *file,
                     const char *const argv[], const char *const envp[]) {
    if (!process || !file || (file->flags & 0xFFU) != VFS_FILE || !file->data) return -1;
    const uint8_t *image = (const uint8_t *)file->data;
    const struct elf64_header *header = (const struct elf64_header *)image;
    if (!valid_header(header, file->length)) return -1;

    const struct elf64_program_header *programs =
        (const struct elf64_program_header *)(image + header->phoff);
    uint64_t image_end = 0;

    for (uint16_t i = 0; i < header->phnum; i++) {
        const struct elf64_program_header *program = &programs[i];
        if (program->type != PT_LOAD) continue;
        if (program->offset > file->length || program->filesz > file->length - program->offset) return -1;
        if (map_segment_pages(process, program) != 0) return -1;
        uint64_t segment_end = program->vaddr + program->memsz;
        if (segment_end > image_end) image_end = segment_end;
    }
    for (uint16_t i = 0; i < header->phnum; i++) {
        const struct elf64_program_header *program = &programs[i];
        if (program->type == PT_LOAD && copy_segment(process, image, program) != 0) return -1;
    }

    uint64_t stack_bottom = USER_STACK_TOP - USER_STACK_PAGES * 4096ULL;
    for (uint64_t address = stack_bottom; address < USER_STACK_TOP; address += 4096) {
        uint64_t physical = (uint64_t)pmm_alloc_page();
        memset(vmm_phys_to_virt(physical), 0, 4096);
        if (vmm_map_page_in(process->cr3, address, physical,
                            PAGE_PRESENT | PAGE_WRITE | PAGE_USER) != 0) return -1;
    }

    if (vmm_translate(process->cr3, header->entry, NULL, NULL) != 0) return -1;
    process->entry = header->entry;
    process->brk_start = align_up(image_end, 4096);
    process->brk_end = process->brk_start;
    process->mmap_base = 0x0000600000000000ULL;
    if (build_initial_stack(process, header, programs, argv, envp) != 0) return -1;
    return 0;
}
