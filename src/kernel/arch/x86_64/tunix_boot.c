#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../../include/boot_framebuffer.h"
#include "../../include/block.h"
#include "../../include/boot_manifest.h"

/*
 * Being loaded by tunix-boot.
 *
 * The kernel's own entry contract has not changed: kmain still takes an E820
 * map, a manifest and a framebuffer description, because that is what the rest
 * of the kernel is written against. What changed is where they come from. The
 * old stage2 built all three in real mode and left them at fixed addresses;
 * this asks the loader for what it has and builds the same three structures out
 * of the answers.
 *
 * Keeping the contract means the change is confined to this file. Nothing below
 * kmain knows which loader started it.
 */

#define BOOT_REQUEST_MAGIC_LOW 0x5449554e49582d31ULL
#define BOOT_REQUEST_MAGIC_HIGH 0x424f4f54503a3031ULL

#define BOOT_REQUEST_MEMORY_MAP 1U
#define BOOT_REQUEST_COMMAND_LINE 3U
#define BOOT_REQUEST_FRAMEBUFFER 5U
#define BOOT_REQUEST_ACPI 6U

/* Mirrors the loader's kinds. Only the first is memory the kernel may take. */
#define BOOT_MEMORY_USABLE 0U
#define BOOT_MEMORY_RECLAIMABLE 1U

/* What E820 called them, which is what pmm_init still reads. */
#define E820_USABLE 1U
#define E820_RESERVED 2U

/* The kernel takes these as physical addresses — it converts them itself once
   its own page tables are up — but everything here is a higher-half pointer
   into the kernel image. This is the offset the linker script applied. */
#define KERNEL_VIRTUAL_BASE 0xFFFFFFFF80000000ULL

#define MEMORY_ENTRY_CAPACITY 128U
#define MANIFEST_SECTOR_BYTES 512U

struct boot_request {
    uint64_t magic[2];
    uint64_t id;
    uint64_t revision;
    void *response;
};

struct boot_memory_entry {
    uint64_t base;
    uint64_t length;
    uint64_t kind;
};

struct boot_memory_map_response {
    uint64_t revision;
    uint64_t entry_count;
    struct boot_memory_entry *entries;
};

struct boot_command_line_response {
    uint64_t revision;
    const char *command_line;
};

struct boot_framebuffer_response {
    uint64_t revision;
    uint64_t base;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t bits_per_pixel;
    uint8_t red_shift;
    uint8_t red_bits;
    uint8_t green_shift;
    uint8_t green_bits;
    uint8_t blue_shift;
    uint8_t blue_bits;
    uint8_t reserved[2];
};

/* The loader finds these by scanning the loaded image for the magic pair, so
   they need no fixed address and no header of their own. */
__attribute__((used, aligned(8)))
static struct boot_request memory_map_request = {
    {BOOT_REQUEST_MAGIC_LOW, BOOT_REQUEST_MAGIC_HIGH},
    BOOT_REQUEST_MEMORY_MAP, 0, NULL,
};

__attribute__((used, aligned(8)))
static struct boot_request command_line_request = {
    {BOOT_REQUEST_MAGIC_LOW, BOOT_REQUEST_MAGIC_HIGH},
    BOOT_REQUEST_COMMAND_LINE, 0, NULL,
};

/* The address of the RSDP, which is where the interrupt controllers are
   described. The loader has already decided the tables are believable. */
struct boot_acpi_response {
    uint64_t revision;
    uint64_t rsdp;
};

__attribute__((used, aligned(8)))
static struct boot_request acpi_request = {
    {BOOT_REQUEST_MAGIC_LOW, BOOT_REQUEST_MAGIC_HIGH},
    BOOT_REQUEST_ACPI, 0, NULL,
};

__attribute__((used, aligned(8)))
static struct boot_request framebuffer_request = {
    {BOOT_REQUEST_MAGIC_LOW, BOOT_REQUEST_MAGIC_HIGH},
    BOOT_REQUEST_FRAMEBUFFER, 0, NULL,
};

/* The E820 map pmm_init walks, rebuilt from what the loader described. */
struct e820_entry {
    uint64_t base;
    uint64_t length;
    uint32_t type;
    uint32_t attributes;
} __attribute__((packed));

static struct e820_entry memory_entries[MEMORY_ENTRY_CAPACITY];
static struct boot_framebuffer_info framebuffer_info;
static uint8_t manifest_storage[MANIFEST_SECTOR_BYTES];

void kmain(uint32_t mmap_count, uint64_t mmap_address, uint64_t manifest_address,
           uint64_t framebuffer_info_address);
int ata_pio_read28(uint32_t lba, uint32_t sectors, void *destination);

static uint32_t build_memory_map(void) {
    const struct boot_memory_map_response *response = memory_map_request.response;
    if (!response || !response->entries) return 0;

    uint32_t count = 0;
    for (uint64_t index = 0;
         index < response->entry_count && count < MEMORY_ENTRY_CAPACITY;
         index++) {
        const struct boot_memory_entry *region = &response->entries[index];

        /* Reclaimable memory is the loader's own, and it is finished with it
           the moment the kernel is running — but the responses being read right
           now live there, so it is handed over as reserved and the kernel is
           free to take it later. Usable is the only kind claimed here. */
        memory_entries[count].base = region->base;
        memory_entries[count].length = region->length;
        memory_entries[count].type =
            region->kind == BOOT_MEMORY_USABLE ? E820_USABLE : E820_RESERVED;
        memory_entries[count].attributes = 1;
        count++;
    }
    return count;
}

static void build_framebuffer_info(void) {
    const struct boot_framebuffer_response *response = framebuffer_request.response;
    if (!response || response->base == 0) return;

    framebuffer_info.magic = TUNIX_BOOT_FB_MAGIC;
    framebuffer_info.version = TUNIX_BOOT_FB_VERSION;
    framebuffer_info.size = sizeof framebuffer_info;
    framebuffer_info.physical_address = response->base;
    framebuffer_info.pitch = response->pitch;
    framebuffer_info.width = (uint16_t)response->width;
    framebuffer_info.height = (uint16_t)response->height;
    framebuffer_info.bits_per_pixel = (uint8_t)response->bits_per_pixel;
    framebuffer_info.red_mask_size = response->red_bits;
    framebuffer_info.red_field_position = response->red_shift;
    framebuffer_info.green_mask_size = response->green_bits;
    framebuffer_info.green_field_position = response->green_shift;
    framebuffer_info.blue_mask_size = response->blue_bits;
    framebuffer_info.blue_field_position = response->blue_shift;

    /* The BIOS font the old stage2 left behind is not there under a loader that
       never entered real mode, and the kernel has its own. */
    framebuffer_info.font_physical_address = 0;
}

/* The command line carries where the manifest is, because the loader reads a
   filesystem and the manifest is not in one — it sits at a raw sector, next to
   the initramfs the kernel loads itself. */
static uint32_t manifest_lba_from_command_line(void) {
    const struct boot_command_line_response *response =
        command_line_request.response;
    if (!response || !response->command_line) return 0;

    static const char key[] = "manifest_lba=";
    const char *at = response->command_line;

    for (; *at != '\0'; at++) {
        size_t index = 0;
        while (key[index] != '\0' && at[index] == key[index]) index++;
        if (key[index] != '\0') continue;

        uint32_t value = 0;
        const char *digits = at + index;
        if (*digits < '0' || *digits > '9') return 0;
        while (*digits >= '0' && *digits <= '9') {
            value = value * 10U + (uint32_t)(*digits - '0');
            digits++;
        }
        return value;
    }
    return 0;
}

static uint64_t physical_of(const void *address);

/* Where the manifest was found, so the block layer can ask every other disk
   whether it carries one too and pick the root by evidence. */
uint32_t tunix_boot_manifest_lba(void) { return manifest_lba_from_command_line(); }

/*
 * The first disk read the kernel makes, and the one that decides whether it can
 * boot at all. It goes through the block layer rather than straight to the IDE
 * ports: a machine whose only disk is SATA or NVMe answers here and nowhere
 * else, and "invalid boot manifest" was all it used to be able to say.
 */
static uint64_t read_manifest(void) {
    uint32_t lba = manifest_lba_from_command_line();
    if (lba == 0) return 0;
    block_probe_early();
    if (block_read(lba, 1, manifest_storage) != 0) return 0;

    const struct boot_manifest *manifest =
        (const struct boot_manifest *)manifest_storage;
    if (manifest->magic != TUNIX_MANIFEST_MAGIC) return 0;
    return physical_of(manifest_storage);
}

void tunix_boot_start(void);

static uint64_t physical_of(const void *address) {
    return (uint64_t)(uintptr_t)address - KERNEL_VIRTUAL_BASE;
}

/* Where the firmware's ACPI tables start, or zero on a machine that has none.
   kmain's arguments are what the old bootloader could supply and are not worth
   widening for one pointer, so this is asked for rather than handed over. */
uint64_t tunix_boot_rsdp(void) {
    const struct boot_acpi_response *response = acpi_request.response;
    return response ? response->rsdp : 0;
}

void tunix_boot_start(void) {
    uint32_t count = build_memory_map();
    build_framebuffer_info();

    kmain(count, physical_of(memory_entries), read_manifest(),
          framebuffer_info.magic ? physical_of(&framebuffer_info) : 0);
}
