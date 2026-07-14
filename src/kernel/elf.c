#include <stddef.h>
#include <stdint.h>
#include "include/elf.h"
#include "include/kstring.h"
#include "include/pmm.h"
#include "include/process.h"
#include "include/random.h"
#include "include/vfs.h"
#include "include/vmm.h"

#define ELFCLASS64 2
#define ELFDATA2LSB 1
#define EV_CURRENT 1
#define ET_EXEC 2
#define ET_DYN 3
#define EM_X86_64 62
#define PT_LOAD 1
#define PT_INTERP 3
#define PF_X 1
#define PF_W 2
#define USER_STACK_TOP 0x00007FFFFFF00000ULL
#define USER_STACK_PAGES 32ULL
#define MAIN_PIE_BASE 0x0000550000000000ULL
#define INTERP_BASE 0x00007F0000000000ULL
#define DEFAULT_MMAP_BASE 0x0000600000000000ULL
#define MAX_ARGC 64
#define MAX_ENVC 64
#define MAX_INTERP_PATH 256

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

struct loaded_elf {
    const struct elf64_header *header;
    const struct elf64_program_header *programs;
    uint64_t load_bias;
    uint64_t entry;
    uint64_t phdr;
    uint64_t image_start;
    uint64_t image_end;
};

static uint64_t align_down(uint64_t value, uint64_t alignment) {
    return value & ~(alignment - 1);
}

static uint64_t align_up(uint64_t value, uint64_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

static int power_of_two(uint64_t value) {
    return value && !(value & (value - 1));
}

static int add_overflows(uint64_t left, uint64_t right) {
    return left > UINT64_MAX - right;
}

static int valid_header(const struct elf64_header *header, uint64_t file_size) {
    if (file_size < sizeof(*header)) return 0;
    if (header->ident[0] != 0x7F || header->ident[1] != 'E' ||
        header->ident[2] != 'L' || header->ident[3] != 'F') return 0;
    if (header->ident[4] != ELFCLASS64 || header->ident[5] != ELFDATA2LSB ||
        header->ident[6] != EV_CURRENT) return 0;
    if ((header->type != ET_EXEC && header->type != ET_DYN) ||
        header->machine != EM_X86_64 || header->version != EV_CURRENT) return 0;
    if (header->ehsize != sizeof(*header) ||
        header->phentsize != sizeof(struct elf64_program_header) ||
        header->phnum == 0) return 0;
    if (header->phoff > file_size) return 0;
    uint64_t table_size = (uint64_t)header->phnum * header->phentsize;
    if (table_size > file_size - header->phoff) return 0;
    return 1;
}

static int validate_programs(const struct elf64_header *header,
                             const struct elf64_program_header *programs,
                             uint64_t file_size,
                             uint64_t *minimum_out, uint64_t *maximum_out) {
    uint64_t minimum = UINT64_MAX;
    uint64_t maximum = 0;
    int load_count = 0;

    for (uint16_t i = 0; i < header->phnum; i++) {
        const struct elf64_program_header *program = &programs[i];
        if (program->type == PT_INTERP) {
            if (program->filesz < 2 || program->filesz > MAX_INTERP_PATH ||
                program->offset > file_size ||
                program->filesz > file_size - program->offset) return -1;
            continue;
        }
        if (program->type != PT_LOAD) continue;
        load_count++;
        if (program->memsz < program->filesz ||
            program->offset > file_size ||
            program->filesz > file_size - program->offset) return -1;
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

static int choose_load_bias(const struct elf64_header *header,
                            uint64_t minimum, uint64_t preferred_base,
                            uint64_t *bias_out) {
    if (header->type == ET_EXEC) {
        *bias_out = 0;
        return minimum < 0x10000ULL ? -1 : 0;
    }
    if (preferred_base < minimum) return -1;
    *bias_out = preferred_base - minimum;
    return 0;
}

static int map_segment_pages(struct process *process,
                             const struct elf64_program_header *program,
                             uint64_t load_bias) {
    if (add_overflows(program->vaddr, load_bias) ||
        add_overflows(program->vaddr + load_bias, program->memsz)) return -1;
    uint64_t virtual_address = program->vaddr + load_bias;
    if (virtual_address < 0x10000ULL || virtual_address >= USER_ADDRESS_LIMIT ||
        program->memsz > USER_ADDRESS_LIMIT - virtual_address) return -1;

    uint64_t start = align_down(virtual_address, 4096);
    uint64_t end = align_up(virtual_address + program->memsz, 4096);
    for (uint64_t address = start; address < end; address += 4096) {
        uint64_t old_flags;
        if (vmm_translate(process->cr3, address, NULL, &old_flags) == 0) {
            if (!(old_flags & PAGE_WRITE) &&
                vmm_protect_page_in(process->cr3, address,
                    old_flags | PAGE_WRITE | PAGE_USER | PAGE_PRESENT) != 0) return -1;
            continue;
        }
        uint64_t physical = (uint64_t)pmm_alloc_page();
        if (!physical) return -1;
        memset(vmm_phys_to_virt(physical), 0, 4096);
        if (vmm_map_page_in(process->cr3, address, physical,
                            PAGE_USER | PAGE_PRESENT | PAGE_WRITE) != 0) {
            pmm_free_page((void *)physical);
            return -1;
        }
    }
    return 0;
}

static int copy_segment(struct process *process, const uint8_t *image,
                        const struct elf64_program_header *program,
                        uint64_t load_bias) {
    uint64_t remaining = program->filesz;
    uint64_t source_offset = program->offset;
    uint64_t destination = program->vaddr + load_bias;
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

static int segment_page_permissions(const struct elf64_header *header,
                                    const struct elf64_program_header *programs,
                                    uint64_t page, uint64_t load_bias,
                                    int *writable_out, int *executable_out) {
    int covered = 0;
    int writable = 0;
    int executable = 0;
    for (uint16_t i = 0; i < header->phnum; i++) {
        const struct elf64_program_header *program = &programs[i];
        if (program->type != PT_LOAD || !program->memsz) continue;
        uint64_t start = align_down(program->vaddr + load_bias, 4096);
        uint64_t end = align_up(program->vaddr + load_bias + program->memsz, 4096);
        if (page < start || page >= end) continue;
        covered = 1;
        if (program->flags & PF_W) writable = 1;
        if (program->flags & PF_X) executable = 1;
    }
    *writable_out = writable;
    *executable_out = executable;
    return covered;
}

static int protect_image_pages(struct process *process,
                               const struct loaded_elf *loaded) {
    for (uint64_t page = loaded->image_start; page < loaded->image_end; page += 4096) {
        int writable, executable;
        if (!segment_page_permissions(loaded->header, loaded->programs, page,
                                      loaded->load_bias, &writable, &executable)) continue;
        (void)executable;
        uint64_t old_flags;
        if (vmm_translate(process->cr3, page, NULL, &old_flags) != 0) return -1;
        uint64_t flags = PAGE_USER | PAGE_PRESENT | (old_flags & PAGE_DEVICE);
        if (writable) flags |= PAGE_WRITE;
        if (vmm_protect_page_in(process->cr3, page, flags) != 0) return -1;
    }
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

static int load_image(struct process *process, struct vfs_node *file,
                      uint64_t preferred_base, struct loaded_elf *loaded) {
    if (!process || !file || !loaded ||
        (file->flags & 0xFFU) != VFS_FILE || !file->data) return -1;
    const uint8_t *image = (const uint8_t *)file->data;
    const struct elf64_header *header = (const struct elf64_header *)image;
    if (!valid_header(header, file->length)) return -1;
    const struct elf64_program_header *programs =
        (const struct elf64_program_header *)(image + header->phoff);

    uint64_t minimum, maximum, load_bias;
    if (validate_programs(header, programs, file->length, &minimum, &maximum) != 0 ||
        choose_load_bias(header, minimum, preferred_base, &load_bias) != 0) return -1;
    if (add_overflows(maximum, load_bias) || maximum + load_bias > USER_ADDRESS_LIMIT) return -1;

    for (uint16_t i = 0; i < header->phnum; i++) {
        if (programs[i].type == PT_LOAD &&
            map_segment_pages(process, &programs[i], load_bias) != 0) return -1;
    }
    for (uint16_t i = 0; i < header->phnum; i++) {
        if (programs[i].type == PT_LOAD &&
            copy_segment(process, image, &programs[i], load_bias) != 0) return -1;
    }

    loaded->header = header;
    loaded->programs = programs;
    loaded->load_bias = load_bias;
    loaded->entry = header->entry + load_bias;
    loaded->phdr = program_header_virtual(header, programs, load_bias);
    loaded->image_start = minimum + load_bias;
    loaded->image_end = maximum + load_bias;
    /*
     * AT_PHDR is mandatory for dynamically linked main programs, but older
     * Tunix static binaries were linked with a script that did not place the
     * ELF/program headers in a PT_LOAD segment.  Accept those binaries here;
     * elf_load_process() performs the stricter check after PT_INTERP is known.
     */
    if (loaded->entry < 0x10000ULL ||
        loaded->entry >= USER_ADDRESS_LIMIT ||
        vmm_translate(process->cr3, loaded->entry, NULL, NULL) != 0) return -1;
    return protect_image_pages(process, loaded);
}

static int interpreter_path(const uint8_t *image,
                            const struct elf64_header *header,
                            const struct elf64_program_header *programs,
                            char path[MAX_INTERP_PATH]) {
    const struct elf64_program_header *interp = NULL;
    for (uint16_t i = 0; i < header->phnum; i++) {
        if (programs[i].type != PT_INTERP) continue;
        if (interp) return -1;
        interp = &programs[i];
    }
    if (!interp) {
        path[0] = '\0';
        return 0;
    }
    if (interp->filesz < 2 || interp->filesz > MAX_INTERP_PATH) return -1;
    const uint8_t *source = image + interp->offset;
    if (source[interp->filesz - 1] != '\0') return -1;
    memcpy(path, source, (size_t)interp->filesz);
    if (path[0] != '/' || strlen(path) + 1 != interp->filesz) return -1;
    return 1;
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

static int build_initial_stack(struct process *process,
                               const struct loaded_elf *main_image,
                               uint64_t interpreter_base,
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
    uint8_t random_bytes[16];
    random_get_bytes(random_bytes, sizeof(random_bytes));
    if (push_bytes(process, &sp, random_bytes, sizeof(random_bytes)) != 0) return -1;
    memset(random_bytes, 0, sizeof(random_bytes));
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
        {AT_ENTRY, main_image->entry},
        {AT_FLAGS, 0},
        {AT_BASE, interpreter_base},
        {AT_PHNUM, main_image->header->phnum},
        {AT_PHENT, main_image->header->phentsize},
        {AT_PHDR, main_image->phdr},
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
    for (size_t i = envc; i > 0; i--)
        if (push_u64(process, &sp, env_addresses[i - 1]) != 0) return -1;
    if (push_u64(process, &sp, 0) != 0) return -1;
    for (size_t i = argc; i > 0; i--)
        if (push_u64(process, &sp, argv_addresses[i - 1]) != 0) return -1;
    if (push_u64(process, &sp, argc) != 0) return -1;

    if (sp & 15ULL) return -1;
    process->user_stack_top = sp;
    return 0;
}

int elf_load_process(struct process *process, struct vfs_node *file,
                     const char *const argv[], const char *const envp[]) {
    if (!process || !file || (file->flags & 0xFFU) != VFS_FILE || !file->data) return -1;

    if (file->length < sizeof(struct elf64_header)) return -1;
    struct loaded_elf main_image;
    memset(&main_image, 0, sizeof(main_image));
    const struct elf64_header *main_header = (const struct elf64_header *)file->data;
    uint64_t main_base = main_header->type == ET_DYN ? MAIN_PIE_BASE : 0;
    if (load_image(process, file, main_base, &main_image) != 0) return -1;

    char interp_path[MAX_INTERP_PATH];
    int interp_status = interpreter_path((const uint8_t *)file->data,
                                         main_image.header, main_image.programs,
                                         interp_path);
    if (interp_status < 0) return -1;
    /* musl's dynamic linker consumes AT_PHDR for the main executable. */
    if (interp_status > 0 && !main_image.phdr) return -1;

    struct loaded_elf interpreter;
    memset(&interpreter, 0, sizeof(interpreter));
    uint64_t initial_entry = main_image.entry;
    uint64_t interpreter_base = 0;
    if (interp_status > 0) {
        struct vfs_node *interp_file = vfs_lookup(interp_path);
        if (!interp_file || load_image(process, interp_file, INTERP_BASE, &interpreter) != 0 ||
            interpreter.header->type != ET_DYN) return -1;
        char nested_path[MAX_INTERP_PATH];
        if (interpreter_path((const uint8_t *)interp_file->data,
                             interpreter.header, interpreter.programs,
                             nested_path) != 0) return -1;
        initial_entry = interpreter.entry;
        interpreter_base = interpreter.load_bias;
    }

    uint64_t stack_bottom = USER_STACK_TOP - USER_STACK_PAGES * 4096ULL;
    for (uint64_t address = stack_bottom; address < USER_STACK_TOP; address += 4096) {
        uint64_t physical = (uint64_t)pmm_alloc_page();
        if (!physical) return -1;
        memset(vmm_phys_to_virt(physical), 0, 4096);
        if (vmm_map_page_in(process->cr3, address, physical,
                            PAGE_PRESENT | PAGE_WRITE | PAGE_USER) != 0) {
            pmm_free_page((void *)physical);
            return -1;
        }
    }

    process->entry = initial_entry;
    process->brk_start = align_up(main_image.image_end, 4096);
    process->brk_end = process->brk_start;
    process->mmap_base = DEFAULT_MMAP_BASE;
    if (build_initial_stack(process, &main_image, interpreter_base, argv, envp) != 0)
        return -1;
    return 0;
}
