#ifndef TUNIX_ELF_H
#define TUNIX_ELF_H

#include <stdint.h>

struct file;
struct process;
struct vfs_node;

// Segment pages an exec maps from the page cache when the program first
// touches them, so every process running a binary shares one copy of it.
#define ELF_IMAGE_AREAS 16
struct elf_image_area {
    uint64_t start, end, page_flags, offset;
    struct file *file;
};
struct elf_image_areas {
    unsigned count;
    struct elf_image_area area[ELF_IMAGE_AREAS];
};

// With areas NULL every segment is copied in at once.
int elf_load_process(struct process *process, struct vfs_node *file, const char *const argv[],
                     const char *const envp[], struct elf_image_areas *areas);
void elf_image_areas_release(struct elf_image_areas *areas);

#endif
