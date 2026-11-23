#include <stddef.h>
#include <stdint.h>
#include <tunix/elf.h>
#include <tunix/elf_plan.h>
#include <tunix/file.h>
#include <tunix/heap.h>
#include <tunix/hwcap.h>
#include <tunix/kstring.h>
#include <tunix/pmm.h>
#include <tunix/process_arch.h>
#include <tunix/process.h>
#include <tunix/random.h>
#include <tunix/signal.h>
#include <tunix/vfs.h>
#include <tunix/vmm.h>

#define ET_DYN     3
#define EM_X86_64  62
#define EM_AARCH64 183

#if defined(__x86_64__)
#define ELF_MACHINE  EM_X86_64
#define ELF_PLATFORM "x86_64"
#elif defined(__aarch64__)
#define ELF_MACHINE  EM_AARCH64
#define ELF_PLATFORM "aarch64"
#endif
#define ELF_HEADER_SIZE   64
#define ELF_PROGRAM_SIZE  56
#define MAIN_PIE_BASE     0x0000550000000000ULL
#define INTERP_BASE       0x00007F0000000000ULL
#define DEFAULT_MMAP_BASE 0x0000600000000000ULL

#define AT_NULL     0
#define AT_PHDR     3
#define AT_PHENT    4
#define AT_PHNUM    5
#define AT_PAGESZ   6
#define AT_BASE     7
#define AT_FLAGS    8
#define AT_ENTRY    9
#define AT_UID      11
#define AT_EUID     12
#define AT_GID      13
#define AT_EGID     14
#define AT_PLATFORM 15
#define AT_HWCAP    16
#define AT_CLKTCK   17
#define AT_SECURE   23
#define AT_RANDOM   25
#define AT_HWCAP2   26
#define AT_EXECFN   31
#define AT_MINSIGSTKSZ 51

struct loaded_elf {
    struct tunix_elf_plan plan;
    struct tunix_elf_segment *segments;
};

static uint64_t align_up(uint64_t value, uint64_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

static void release_image(struct loaded_elf *loaded) {
    kfree(loaded->segments);
    loaded->segments = NULL;
}

static int map_segment_pages(struct process *process, const struct tunix_elf_segment *segment,
                             uint64_t from) {
    for (uint64_t address = from; address < segment->page_end; address += 4096) {
        uint64_t old_flags;
        if (vmm_translate(process->cr3, address, NULL, &old_flags) == 0) {
            if (!(old_flags & PAGE_WRITE) &&
                vmm_protect_page_in(process->cr3, address,
                                    old_flags | PAGE_WRITE | PAGE_USER | PAGE_PRESENT) != 0)
                return -1;
            continue;
        }
        uint64_t physical = process_alloc_user_page();
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

static int copy_segment(struct process *process, struct vfs_node *file,
                        const struct tunix_elf_segment *segment, uint64_t from) {
    uint64_t skip = from > segment->address ? from - segment->address : 0;
    if (skip >= segment->file_size) return 0;
    uint64_t remaining = segment->file_size - skip;
    uint64_t source_offset = segment->file_offset + skip;
    uint64_t destination = segment->address + skip;
    while (remaining) {
        uint64_t physical;
        if (vmm_translate(process->cr3, destination, &physical, NULL) != 0) return -1;
        uint64_t chunk = 4096 - (destination & 0xFFFULL);
        if (chunk > remaining) chunk = remaining;
        if (vfs_read(file, source_offset, (size_t)chunk, vmm_phys_to_virt(physical)) !=
            (int64_t)chunk)
            return -1;
        destination += chunk;
        source_offset += chunk;
        remaining -= chunk;
    }
    return 0;
}

static int page_writable(const struct loaded_elf *loaded, uint64_t page, int *writable_out) {
    int covered = 0;
    int writable = 0;
    for (uint16_t i = 0; i < loaded->plan.load_count; i++) {
        const struct tunix_elf_segment *segment = &loaded->segments[i];
        if (!segment->memory_size || page < segment->page_start || page >= segment->page_end)
            continue;
        covered = 1;
        if (segment->writable) writable = 1;
    }
    *writable_out = writable;
    return covered;
}

static int protect_image_pages(struct process *process, const struct loaded_elf *loaded) {
    for (uint64_t page = loaded->plan.image_start; page < loaded->plan.image_end; page += 4096) {
        int writable;
        if (!page_writable(loaded, page, &writable)) continue;
        uint64_t old_flags;
        // Pages left to the page cache are not mapped yet; their area sets them.
        if (vmm_translate(process->cr3, page, NULL, &old_flags) != 0) continue;
        uint64_t flags = PAGE_USER | PAGE_PRESENT | (old_flags & PAGE_DEVICE);
        if (writable) flags |= PAGE_WRITE;
        if (vmm_protect_page_in(process->cr3, page, flags) != 0) return -1;
    }
    return 0;
}

static uint8_t *read_headers(struct vfs_node *file, size_t *length_out) {
    uint8_t probe[ELF_HEADER_SIZE];
    if (file->length < sizeof(probe)) return NULL;
    if (vfs_read(file, 0, sizeof(probe), probe) != (int64_t)sizeof(probe)) return NULL;
    int64_t span = tunix_elf_header_span(probe, sizeof(probe), file->length, ELF_MACHINE);
    if (span < (int64_t)sizeof(probe)) return NULL;
    uint8_t *headers = (uint8_t *)kmalloc((size_t)span);
    if (!headers) return NULL;
    if (vfs_read(file, 0, (size_t)span, headers) != span) {
        kfree(headers);
        return NULL;
    }
    *length_out = (size_t)span;
    return headers;
}

static int plan_image(struct vfs_node *file, uint64_t preferred_base, struct loaded_elf *loaded) {
    size_t length = 0;
    uint8_t *headers = read_headers(file, &length);
    if (!headers) return -1;
    int status = tunix_elf_plan(headers, length, file->length, ELF_MACHINE, preferred_base,
                                USER_ADDRESS_LIMIT, &loaded->plan, NULL, 0);
    if (status == 0) {
        loaded->segments = (struct tunix_elf_segment *)kmalloc((size_t)loaded->plan.load_count *
                                                               sizeof(*loaded->segments));
        if (!loaded->segments ||
            tunix_elf_plan(headers, length, file->length, ELF_MACHINE, preferred_base,
                           USER_ADDRESS_LIMIT, &loaded->plan, loaded->segments,
                           loaded->plan.load_count) != 0)
            status = -1;
    }
    kfree(headers);
    if (status != 0) release_image(loaded);
    return status;
}

// Where a segment's pages stop coming from the page cache: a last page that
// holds both file data and zeroed memory is copied, as are the zeroed pages.
static uint64_t shared_end(const struct tunix_elf_segment *segment) {
    if (!segment->file_size || (segment->address & 0xFFFULL) != (segment->file_offset & 0xFFFULL))
        return segment->page_start;
    uint64_t data_end = segment->address + segment->file_size;
    uint64_t end = segment->memory_size > segment->file_size ? data_end & ~0xFFFULL
                                                              : align_up(data_end, 4096);
    return end > segment->page_end ? segment->page_end : end;
}

static int segments_share_pages(const struct loaded_elf *loaded) {
    for (uint16_t i = 0; i < loaded->plan.load_count; i++)
        for (uint16_t j = i + 1; j < loaded->plan.load_count; j++) {
            const struct tunix_elf_segment *a = &loaded->segments[i], *b = &loaded->segments[j];
            if (a->page_start < b->page_end && b->page_start < a->page_end) return 1;
        }
    return 0;
}

static int load_image(struct process *process, struct vfs_node *file, uint64_t preferred_base,
                      struct loaded_elf *loaded, struct elf_image_areas *areas) {
    if (!process || !file || !loaded || (file->flags & 0xFFU) != VFS_FILE) return -1;
    if (plan_image(file, preferred_base, loaded) != 0) return -1;
    if (areas && (segments_share_pages(loaded) ||
                  areas->count + loaded->plan.load_count > ELF_IMAGE_AREAS))
        areas = NULL;
    struct file *backing = areas ? file_open_node(file, 0 /* O_RDONLY */) : NULL;
    if (!backing) areas = NULL;
    int status = 0;
    for (uint16_t i = 0; status == 0 && i < loaded->plan.load_count; i++) {
        const struct tunix_elf_segment *segment = &loaded->segments[i];
        uint64_t from = areas ? shared_end(segment) : segment->page_start;
        if (from > segment->page_start) {
            struct elf_image_area *area = &areas->area[areas->count++];
            area->start = segment->page_start;
            area->end = from;
            area->page_flags = segment->writable ? PAGE_WRITE : 0;
            area->offset = segment->file_offset - (segment->address - segment->page_start);
            area->file = backing;
            file_ref(backing);
        }
        status = map_segment_pages(process, segment, from);
        if (status == 0) status = copy_segment(process, file, segment, from);
    }
    if (backing) file_unref(backing);
    if (status == 0 &&
        (loaded->plan.entry < loaded->plan.image_start || loaded->plan.entry >= loaded->plan.image_end))
        status = -1;
    if (status == 0) status = protect_image_pages(process, loaded);
    if (status != 0) release_image(loaded);
    return status;
}

void elf_image_areas_release(struct elf_image_areas *areas) {
    if (!areas) return;
    for (unsigned i = 0; i < areas->count; i++) file_unref(areas->area[i].file);
    areas->count = 0;
}

static int interpreter_path(struct vfs_node *file, const struct tunix_elf_plan *plan, char *path) {
    if (!plan->interp_size) {
        path[0] = '\0';
        return 0;
    }
    if (plan->interp_size > VFS_PATH_MAX) return -1;
    if (vfs_read(file, plan->interp_offset, (size_t)plan->interp_size, path) !=
        (int64_t)plan->interp_size)
        return -1;
    return tunix_elf_interp_valid((const uint8_t *)path, (size_t)plan->interp_size) ? 1 : -1;
}

static int push_bytes(struct process *process, uint64_t *sp, const void *data, size_t size) {
    if (*sp < size) return -1;
    *sp -= size;
    return vmm_copy_to_space(process->cr3, *sp, data, size);
}

static int push_u64(struct process *process, uint64_t *sp, uint64_t value) {
    return push_bytes(process, sp, &value, sizeof(value));
}

static int place_initial_stack(struct process *process, const struct loaded_elf *main_image,
                               uint64_t interpreter_base, const char *const argv[],
                               const char *const envp[], size_t argc, size_t envc,
                               uint64_t *argv_addresses, uint64_t *env_addresses) {
    uint64_t sp = USER_STACK_TOP;
    static const char platform[] = ELF_PLATFORM;
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
    uint64_t arg_end = sp;
    for (size_t i = argc; i > 0; i--) {
        size_t length = strlen(argv[i - 1]) + 1;
        if (push_bytes(process, &sp, argv[i - 1], length) != 0) return -1;
        argv_addresses[i - 1] = sp;
    }
    process->arg_start = argc ? argv_addresses[0] : arg_end;
    process->arg_end = arg_end;
    process->env_end = platform_address;

    sp &= ~15ULL;
    uint64_t execfn = argc ? argv_addresses[0] : 0;
    // The stack a signal frame needs, FPU state included, for sigaltstack sizes.
#if defined(__x86_64__)
    uint64_t signal_frame = arch_fpu_signal_size() + 64U + SIGNAL_CONTEXT_SIZE + SIGNAL_SIGINFO_SIZE + 32U;
#else
    uint64_t signal_frame = SIGNAL_CONTEXT_SIZE + SIGNAL_SIGINFO_SIZE + 32U;
#endif
    const uint64_t auxv[][2] = {{AT_NULL, 0},
                                {AT_MINSIGSTKSZ, signal_frame},
                                {AT_EXECFN, execfn},
                                {AT_HWCAP2, 0},
                                {AT_RANDOM, random_address},
                                {AT_SECURE, 0},
                                {AT_CLKTCK, 100},
                                {AT_HWCAP, arch_elf_hwcap()},
                                {AT_PLATFORM, platform_address},
                                {AT_EGID, 0},
                                {AT_GID, 0},
                                {AT_EUID, 0},
                                {AT_UID, 0},
                                {AT_ENTRY, main_image->plan.entry},
                                {AT_FLAGS, 0},
                                {AT_BASE, interpreter_base},
                                {AT_PHNUM, main_image->plan.phnum},
                                {AT_PHENT, ELF_PROGRAM_SIZE},
                                {AT_PHDR, main_image->plan.phdr},
                                {AT_PAGESZ, 4096}};
    const size_t aux_count = sizeof(auxv) / sizeof(auxv[0]);
    size_t stack_words = aux_count * 2 + 1 + envc + 1 + argc + 1;
    if ((sp - stack_words * sizeof(uint64_t)) & 15ULL) {
        if (push_u64(process, &sp, 0) != 0) return -1;
    }
    for (size_t i = 0; i < aux_count; i++) {
        if (push_u64(process, &sp, auxv[i][1]) != 0 || push_u64(process, &sp, auxv[i][0]) != 0)
            return -1;
    }
    process->auxv_start = sp;
    process->auxv_end = sp + aux_count * 2 * sizeof(uint64_t);

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

static int build_initial_stack(struct process *process, const struct loaded_elf *main_image,
                               uint64_t interpreter_base, const char *const argv[],
                               const char *const envp[]) {
    size_t argc = 0, envc = 0;
    while (argv && argv[argc]) argc++;
    while (envp && envp[envc]) envc++;

    uint64_t *addresses = (uint64_t *)kmalloc((argc + envc + 1) * sizeof(uint64_t));
    if (!addresses) return -1;
    int status = place_initial_stack(process, main_image, interpreter_base, argv, envp, argc, envc,
                                     addresses, addresses + argc);
    kfree(addresses);
    return status;
}

static struct vfs_node *lookup_under_root(const struct process *process, const char *path) {
    if (!process || !process->root) return vfs_lookup(path);
    VFS_PATH_SCOPED full = (char *)kmalloc(2 * VFS_PATH_MAX);
    if (!full || vfs_node_path(process->root, full, VFS_PATH_MAX) != 0) return NULL;
    size_t length = strlen(full);
    while (length > 1 && full[length - 1] == '/') full[--length] = '\0';
    if (length <= 1) return vfs_lookup(path);
    size_t tail = strlen(path);
    if (length + tail + 1 > 2 * VFS_PATH_MAX) return NULL;
    memcpy(full + length, path, tail + 1);
    return vfs_lookup(full);
}

int elf_load_process(struct process *process, struct vfs_node *file, const char *const argv[],
                     const char *const envp[], struct elf_image_areas *areas) {
    if (!process || !file || (file->flags & 0xFFU) != VFS_FILE) return -1;

    struct loaded_elf main_image;
    struct loaded_elf interpreter;
    memset(&main_image, 0, sizeof(main_image));
    memset(&interpreter, 0, sizeof(interpreter));
    int status = -1;
    if (load_image(process, file, MAIN_PIE_BASE, &main_image, areas) != 0) return -1;

    VFS_PATH_SCOPED interp_path = vfs_path_buffer();
    if (!interp_path) goto done;
    int interp_status = interpreter_path(file, &main_image.plan, interp_path);
    if (interp_status < 0) goto done;

    uint64_t initial_entry = main_image.plan.entry;
    uint64_t interpreter_base = 0;
    if (interp_status > 0) {
        struct vfs_node *interp_file = lookup_under_root(process, interp_path);
        if (!interp_file || load_image(process, interp_file, INTERP_BASE, &interpreter, areas) != 0 ||
            interpreter.plan.kind != ET_DYN || interpreter.plan.interp_size)
            goto done;
        initial_entry = interpreter.plan.entry;
        interpreter_base = interpreter.plan.load_bias;
    }

    uint64_t stack_bytes = 4096ULL;
    for (size_t index = 0; argv && argv[index]; index++) stack_bytes += strlen(argv[index]) + 1 + 8;
    for (size_t index = 0; envp && envp[index]; index++) stack_bytes += strlen(envp[index]) + 1 + 8;
    uint64_t stack_pages = (stack_bytes + 4095ULL) / 4096ULL + USER_STACK_INITIAL_PAGES;
    uint64_t stack_bottom = USER_STACK_TOP - stack_pages * 4096ULL;
    if (stack_bottom < process_stack_floor(process)) goto done;
    for (uint64_t address = stack_bottom; address < USER_STACK_TOP; address += 4096) {
        uint64_t physical = process_alloc_user_page();
        if (!physical) goto done;
        memset(vmm_phys_to_virt(physical), 0, 4096);
        if (vmm_map_page_in(process->cr3, address, physical,
                            PAGE_PRESENT | PAGE_WRITE | PAGE_USER) != 0) {
            pmm_free_page((void *)physical);
            goto done;
        }
    }

    if (arch_map_signal_trampoline(process->cr3) != 0) goto done;

    process->entry = initial_entry;
    process->brk_start = align_up(main_image.plan.image_end, 4096);
    process->brk_end = process->brk_start;
    process->mmap_base = DEFAULT_MMAP_BASE;
    if (build_initial_stack(process, &main_image, interpreter_base, argv, envp) == 0) status = 0;

done:
    release_image(&main_image);
    release_image(&interpreter);
    if (status != 0) elf_image_areas_release(areas);
    return status;
}
