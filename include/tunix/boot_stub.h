#ifndef TUNIX_BOOT_STUB_H
#define TUNIX_BOOT_STUB_H

#include <stdint.h>

#include <tunix/boot.h>
#include <tunix/boot_framebuffer.h>

// What arch/x86_64/bzimage hands the kernel when it was booted through the Linux boot
// protocol or as an EFI application rather than by Limine. The stub enters
// stub_kernel_entry with the address of this structure, in the direct map at
// hhdm_offset, in rdi; the kernel copies it before it touches any memory.

#define BOOT_STUB_MAGIC        0x4255535458494E55ULL /* "UNIXTSUB" */
#define BOOT_STUB_COMMAND_LINE 4096U

struct boot_stub_handoff {
    uint64_t magic;
    uint64_t kernel_physical_base;
    uint64_t hhdm_offset;
    uint64_t rsdp;
    uint32_t memory_count;
    uint32_t has_framebuffer;
    struct boot_memory_region memory[BOOT_MEMORY_REGIONS];
    struct boot_framebuffer_info framebuffer;
    char command_line[BOOT_STUB_COMMAND_LINE];
    uint64_t initramfs_physical; // the boot protocol's ramdisk, 0 if none
    uint64_t initramfs_size;
};

#endif
