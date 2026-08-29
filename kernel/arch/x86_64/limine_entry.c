/*
 * Entering from Limine.
 *
 * Tunix used to ship its own bootloader, which meant every change to the boot
 * contract was two projects wide and the kernel could only be started from a
 * disk that loader had written. Limine is the boot protocol this replaces it
 * with: it hands over a memory map, a linear framebuffer, the ACPI tables and
 * a command line, on both BIOS and UEFI, and it maps the image itself.
 *
 * Nothing below kmain knows any of that. This file turns Limine's responses
 * into the single struct boot_info the rest of the kernel reads.
 */

#define LIMINE_API_REVISION 3
#include <limine.h>

#include <stddef.h>
#include <stdint.h>

#include "../../include/boot.h"
#include "../../include/boot_framebuffer.h"

/*
 * The request block. Limine finds these by scanning the loaded image between
 * the two markers, which is why they are `used` and live in a section of their
 * own that the linker script keeps.
 */
__attribute__((used, section(".limine_requests")))
static volatile LIMINE_BASE_REVISION(3);

/* Four levels, on a machine that could do five. Every address constant in
   vmm.h is a four-level one, and the kernel walks the tables itself. */
__attribute__((used, section(".limine_requests")))
static volatile struct limine_paging_mode_request paging_mode_request = {
    .id = LIMINE_PAGING_MODE_REQUEST, .revision = 0, .response = NULL,
    .mode = LIMINE_PAGING_MODE_X86_64_4LVL,
    .max_mode = LIMINE_PAGING_MODE_X86_64_4LVL,
    .min_mode = LIMINE_PAGING_MODE_X86_64_4LVL,
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_memmap_request memmap_request = {
    .id = LIMINE_MEMMAP_REQUEST, .revision = 0, .response = NULL,
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_hhdm_request hhdm_request = {
    .id = LIMINE_HHDM_REQUEST, .revision = 0, .response = NULL,
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_framebuffer_request framebuffer_request = {
    .id = LIMINE_FRAMEBUFFER_REQUEST, .revision = 0, .response = NULL,
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_executable_address_request address_request = {
    .id = LIMINE_EXECUTABLE_ADDRESS_REQUEST, .revision = 0, .response = NULL,
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_executable_cmdline_request cmdline_request = {
    .id = LIMINE_EXECUTABLE_CMDLINE_REQUEST, .revision = 0, .response = NULL,
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_rsdp_request rsdp_request = {
    .id = LIMINE_RSDP_REQUEST, .revision = 0, .response = NULL,
};

__attribute__((used, section(".limine_requests_start")))
static volatile LIMINE_REQUESTS_START_MARKER;

__attribute__((used, section(".limine_requests_end")))
static volatile LIMINE_REQUESTS_END_MARKER;

/* Where the linker put the image, so its extent can be reported without asking
   the loader for a size it does not publish. */
extern uint8_t kernel_image_start[];
extern uint8_t kernel_reserve_end[];

extern void kmain(const struct boot_info *boot);
extern void panic(const char *message);

static struct boot_memory_region memory_regions[BOOT_MEMORY_REGIONS];
static struct boot_framebuffer_info framebuffer_info;
static struct boot_info info;

void limine_start(void);

/*
 * Bootloader-reclaimable memory is the loader's own, and the protocol says the
 * kernel may take it back. It does not: the page tables the kernel keeps using
 * are in there, and so is every response being read right now. Calling it
 * unusable costs a few hundred kilobytes and removes a whole class of "the
 * machine boots except when it doesn't".
 */
static uint32_t build_memory_map(void) {
    const struct limine_memmap_response *response = memmap_request.response;
    if (!response) return 0;

    uint32_t count = 0;
    for (uint64_t index = 0;
         index < response->entry_count && count < BOOT_MEMORY_REGIONS; index++) {
        const struct limine_memmap_entry *entry = response->entries[index];
        memory_regions[count].base = entry->base;
        memory_regions[count].length = entry->length;
        memory_regions[count].usable = entry->type == LIMINE_MEMMAP_USABLE;
        count++;
    }
    return count;
}

static void build_framebuffer_info(uint64_t hhdm_offset) {
    const struct limine_framebuffer_response *response = framebuffer_request.response;
    if (!response || response->framebuffer_count == 0) return;

    const struct limine_framebuffer *framebuffer = response->framebuffers[0];
    if (!framebuffer->address) return;

    framebuffer_info.magic = TUNIX_BOOT_FB_MAGIC;
    framebuffer_info.version = TUNIX_BOOT_FB_VERSION;
    framebuffer_info.size = sizeof framebuffer_info;
    /* Limine hands over the framebuffer through its own higher-half map; the
       kernel maps it itself, write-combining, and wants the physical address. */
    framebuffer_info.physical_address = (uint64_t)framebuffer->address - hhdm_offset;
    framebuffer_info.pitch = (uint32_t)framebuffer->pitch;
    framebuffer_info.width = (uint16_t)framebuffer->width;
    framebuffer_info.height = (uint16_t)framebuffer->height;
    framebuffer_info.bits_per_pixel = (uint8_t)framebuffer->bpp;
    framebuffer_info.red_mask_size = framebuffer->red_mask_size;
    framebuffer_info.red_field_position = framebuffer->red_mask_shift;
    framebuffer_info.green_mask_size = framebuffer->green_mask_size;
    framebuffer_info.green_field_position = framebuffer->green_mask_shift;
    framebuffer_info.blue_mask_size = framebuffer->blue_mask_size;
    framebuffer_info.blue_field_position = framebuffer->blue_mask_shift;
    /* The BIOS font the old loader left at a fixed address is not there under a
       loader that never entered real mode. The kernel has its own. */
    framebuffer_info.font_physical_address = 0;
}

const struct boot_info *boot_info(void) { return &info; }

const char *boot_command_line_value(const char *key) {
    const char *at = info.command_line;
    if (!at) return NULL;

    for (; *at != '\0'; at++) {
        /* Only at the start of a word, so root= does not match proot=. */
        if (at != info.command_line && at[-1] != ' ') continue;

        size_t index = 0;
        while (key[index] != '\0' && at[index] == key[index]) index++;
        if (key[index] == '\0' && at[index] == '=') return at + index + 1;
    }
    return NULL;
}

/*
 * Whether a word appears on the command line on its own.
 *
 * The lookup above wants `key=`, which is the right shape for root= and init=
 * and the wrong one for a switch: `nosmp` has nothing to say beyond being
 * there, and asking for its value finds nothing.
 */
int boot_command_line_flag(const char *key) {
    const char *at = info.command_line;
    if (!at) return 0;

    for (; *at != '\0'; at++) {
        if (at != info.command_line && at[-1] != ' ') continue;
        size_t index = 0;
        while (key[index] != '\0' && at[index] == key[index]) index++;
        if (key[index] != '\0') continue;
        if (at[index] == '\0' || at[index] == ' ') return 1;
    }
    return 0;
}

/*
 * Whether `verbose` was asked for.
 *
 * It turns on a bounded trace of the first steps userland takes -- the first
 * faults and the first syscalls -- and nothing else. That is the one window
 * the kernel has no other way to show: a process that never reaches its first
 * syscall and never takes an unhandled fault is, from outside, a machine that
 * printed its last line and stopped.
 */
int boot_verbose(void) {
    static int cached = -1;
    if (cached < 0) cached = boot_command_line_flag("verbose");
    return cached;
}

void limine_start(void) {
    if (!LIMINE_BASE_REVISION_SUPPORTED) panic("limine: base revision 3 unsupported");
    if (!hhdm_request.response) panic("limine: no higher-half direct map");
    if (paging_mode_request.response &&
        paging_mode_request.response->mode != LIMINE_PAGING_MODE_X86_64_4LVL)
        panic("limine: five-level paging is not supported");
    if (!address_request.response) panic("limine: no executable address");

    info.hhdm_offset = hhdm_request.response->offset;
    info.kernel_physical_base = address_request.response->physical_base;
    info.kernel_virtual_base = address_request.response->virtual_base;
    info.kernel_size = (uint64_t)(kernel_reserve_end - kernel_image_start);

    info.memory = memory_regions;
    info.memory_count = build_memory_map();

    build_framebuffer_info(info.hhdm_offset);
    info.framebuffer = framebuffer_info.magic ? &framebuffer_info : NULL;

    info.command_line = cmdline_request.response && cmdline_request.response->cmdline
                            ? cmdline_request.response->cmdline
                            : "";
    info.rsdp = rsdp_request.response ? rsdp_request.response->address : 0;

    kmain(&info);
}
