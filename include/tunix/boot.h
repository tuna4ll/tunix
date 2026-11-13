#ifndef TUNIX_BOOT_H
#define TUNIX_BOOT_H

#include <stdint.h>

#include <tunix/boot_framebuffer.h>

struct boot_memory_region {
    uint64_t base;
    uint64_t length;
    uint32_t usable;
};

#define BOOT_MEMORY_REGIONS 256

struct boot_info {
    const struct boot_memory_region *memory;
    uint32_t memory_count;
    const struct boot_framebuffer_info *framebuffer;
    const char *command_line;
    uint64_t rsdp;
    uint64_t hhdm_offset;
    uint64_t kernel_physical_base;
    uint64_t kernel_virtual_base;
    uint64_t kernel_size;
};

const struct boot_info *boot_info(void);

const char *boot_command_line_value(const char *key);

int boot_command_line_flag(const char *key);

int boot_verbose(void);

#endif
