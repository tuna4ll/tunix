#ifndef TUNIX_BOOT_H
#define TUNIX_BOOT_H

#include <stdint.h>

#include "boot_framebuffer.h"

/*
 * What the bootloader told the kernel, in the kernel's own terms.
 *
 * Everything below kmain reads this and never learns which protocol filled it
 * in. Today that is Limine (see arch/x86_64/limine_entry.c); the point of the
 * indirection is that the fields here are the ones the kernel actually needs,
 * not the ones a particular loader happens to publish.
 */

struct boot_memory_region {
    uint64_t base;
    uint64_t length;
    /* Non-zero when the allocator may hand these pages out. Everything else --
       firmware tables, the loader's own pages, the kernel image -- is memory
       the machine has but the kernel must not touch. */
    uint32_t usable;
};

#define BOOT_MEMORY_REGIONS 256

struct boot_info {
    const struct boot_memory_region *memory;
    uint32_t memory_count;
    /* NULL on a machine the loader could not get a linear framebuffer out of,
       which the kernel treats as fatal: it has no other way to draw. */
    const struct boot_framebuffer_info *framebuffer;
    /* Never NULL; the empty string when the entry carried no parameters. */
    const char *command_line;
    /* Physical, and zero on a machine with no ACPI tables. */
    uint64_t rsdp;
    /* Where the loader mapped all of physical memory. Only vmm_init() uses it:
       it is how the kernel reaches a page table before its own direct map
       exists. */
    uint64_t hhdm_offset;
    /* Limine places the image where it likes, so "virtual minus KERNEL_BASE"
       is no longer the physical address of a kernel static. These two are. */
    uint64_t kernel_physical_base;
    uint64_t kernel_virtual_base;
    uint64_t kernel_size;
};

const struct boot_info *boot_info(void);

/* The value of `key=` on the kernel command line, or NULL. The result points
   into the loader's copy of the line and is not terminated at the value's end,
   so callers parse forward themselves. */
const char *boot_command_line_value(const char *key);

#endif
