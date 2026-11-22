// What vmlinuz does between the loader and the kernel: whichever way it was started
// -- by a Linux loader with boot_params, or by UEFI as an application -- it collects
// the memory map, the framebuffer, the command line and the RSDP into a
// boot_stub_handoff, maps memory the way Limine would have (the first terabytes
// identity mapped and again at HHDM_OFFSET, the kernel at its link address), and
// enters the kernel. The kernel runs where it was loaded, inside this image.
//
// Everything here runs identity mapped and is position-independent: there are no
// relocations, so no initialized pointers either.

#pragma GCC visibility push(hidden)

#include <stddef.h>
#include <stdint.h>

#include <tunix/boot_stub.h>

#define HHDM_OFFSET 0xFFFF800000000000ULL
#define PAGE        4096ULL
#define PTE_PRESENT 0x001ULL
#define PTE_WRITE   0x002ULL
#define PTE_LARGE   0x080ULL
#define TABLE_PAGES 320U
#define RAW_REGIONS 512U

extern char _payload_start[];
extern char _init_end[];
extern char _kernel_blob[];
extern const uint64_t stub_kernel_info[3];
extern void stub_enter_kernel(uint64_t cr3, uint64_t entry, uint64_t handoff, uint64_t no_execute)
    __attribute__((noreturn));

void *memset(void *destination, int value, size_t length);
void *memcpy(void *destination, const void *source, size_t length);

void *memset(void *destination, int value, size_t length) {
    uint8_t *out = destination;
    while (length--) *out++ = (uint8_t)value;
    return destination;
}

void *memcpy(void *destination, const void *source, size_t length) {
    uint8_t *out = destination;
    const uint8_t *in = source;
    while (length--) *out++ = *in++;
    return destination;
}

static struct boot_stub_handoff handoff __attribute__((aligned(4096)));
static uint8_t tables[TABLE_PAGES][PAGE] __attribute__((aligned(4096)));
static unsigned tables_used;
static struct boot_memory_region raw[RAW_REGIONS];
static unsigned raw_count;

// ---- output, for the one thing the stub can say: why it stopped ----

static void outb(uint16_t port, uint8_t value) {
    __asm__ volatile("out dx, al" : : "d"(port), "a"(value));
}

static uint8_t inb(uint16_t port) {
    uint8_t value;
    __asm__ volatile("in al, dx" : "=a"(value) : "d"(port));
    return value;
}

static void serial_write(const char *text) {
    for (; *text; text++) {
        for (unsigned spin = 0; spin < 100000 && !(inb(0x3fd) & 0x20); spin++) {
        }
        outb(0x3f8, (uint8_t)*text);
    }
}

static void (*efi_say)(const char *text);

__attribute__((noreturn)) static void stop(const char *why) {
    serial_write("vmlinuz: ");
    serial_write(why);
    serial_write("\r\n");
    if (efi_say) efi_say(why);
    for (;;) __asm__ volatile("cli; hlt");
}

// ---- the memory map ----

static void add_region(uint64_t base, uint64_t length, int usable) {
    if (!length || raw_count == RAW_REGIONS) return;
    raw[raw_count].base = base;
    raw[raw_count].length = length;
    raw[raw_count].usable = usable ? 1U : 0U;
    raw_count++;
}

static void emit(uint64_t base, uint64_t end, uint32_t usable) {
    if (end <= base || handoff.memory_count == BOOT_MEMORY_REGIONS) return;
    struct boot_memory_region *region = &handoff.memory[handoff.memory_count++];
    region->base = base;
    region->length = end - base;
    region->usable = usable;
}

// Sorts the regions, takes the image and page zero out of usable memory, and
// reports the image as memory in use, as Limine reports the kernel's.
static void finish_memory_map(void) {
    for (unsigned i = 1; i < raw_count; i++) {
        struct boot_memory_region item = raw[i];
        unsigned j = i;
        while (j > 0 && raw[j - 1].base > item.base) {
            raw[j] = raw[j - 1];
            j--;
        }
        raw[j] = item;
    }
    uint64_t image_start = (uint64_t)_payload_start & ~(PAGE - 1);
    uint64_t image_end = ((uint64_t)_init_end + PAGE - 1) & ~(PAGE - 1);
    int image_reported = 0;
    handoff.memory_count = 0;
    for (unsigned index = 0; index < raw_count; index++) {
        uint64_t base = raw[index].base;
        uint64_t end = base + raw[index].length;
        if (!raw[index].usable) {
            emit(base, end, 0);
            continue;
        }
        base = (base + PAGE - 1) & ~(PAGE - 1);
        end &= ~(PAGE - 1);
        if (base < PAGE) base = PAGE;
        if (base < image_start) emit(base, end < image_start ? end : image_start, 1);
        if (!image_reported && base < image_end && end > image_start) {
            emit(image_start, image_end, 0);
            image_reported = 1;
        }
        if (end > image_end) emit(base > image_end ? base : image_end, end, 1);
    }
    if (!image_reported) emit(image_start, image_end, 0);
    if (!handoff.memory_count) stop("the loader gave no memory map");
}

// ---- page tables ----

static uint64_t *table(void) {
    if (tables_used == TABLE_PAGES) stop("not enough room for page tables");
    uint64_t *page = (uint64_t *)tables[tables_used++];
    memset(page, 0, PAGE);
    return page;
}

static uint64_t *child(uint64_t *parent, unsigned index) {
    if (!(parent[index] & PTE_PRESENT))
        parent[index] = (uint64_t)table() | PTE_PRESENT | PTE_WRITE;
    return (uint64_t *)(parent[index] & 0x000FFFFFFFFFF000ULL);
}

static void cpuid(uint32_t leaf, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d) {
    __asm__ volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf), "c"(0));
}

static uint32_t extended_features(void) {
    uint32_t a, b, c, d;
    cpuid(0x80000000U, &a, &b, &c, &d);
    if (a < 0x80000001U) return 0;
    cpuid(0x80000001U, &a, &b, &c, &d);
    return d;
}

__attribute__((noreturn)) static void enter_kernel(void) {
    if (!handoff.has_framebuffer)
        stop("no 32-bit linear framebuffer, which Tunix draws its console on "
             "(with GRUB: set gfxpayload=1280x800x32, or any WIDTHxHEIGHTx32)");
    finish_memory_map();

    uint64_t top = (uint64_t)_init_end;
    for (uint32_t index = 0; index < handoff.memory_count; index++) {
        uint64_t end = handoff.memory[index].base + handoff.memory[index].length;
        if (end > top) top = end;
    }
    uint64_t gigabytes = (top + (1ULL << 30) - 1) >> 30;
    if (gigabytes < 4) gigabytes = 4;
    uint32_t features = extended_features();
    int huge = (features >> 26) & 1;

    // Physical memory, identity mapped for the switch and at HHDM_OFFSET for the
    // kernel, which reads its boot structures and page tables there until it has
    // built its own direct map.
    uint64_t *pml4 = table();
    for (uint64_t gigabyte = 0; gigabyte < gigabytes; gigabyte++) {
        unsigned slot = (unsigned)(gigabyte >> 9);
        if (slot >= 256) break;
        uint64_t *pdpt = child(pml4, slot);
        pml4[256 + slot] = pml4[slot];
        uint64_t base = gigabyte << 30;
        if (huge) {
            pdpt[gigabyte & 511] = base | PTE_PRESENT | PTE_WRITE | PTE_LARGE;
            continue;
        }
        uint64_t *directory = child(pdpt, (unsigned)(gigabyte & 511));
        for (uint64_t entry = 0; entry < 512; entry++)
            directory[entry] = (base + (entry << 21)) | PTE_PRESENT | PTE_WRITE | PTE_LARGE;
    }

    // The kernel at its link address, over the pages it lies in.
    uint64_t virtual_base = stub_kernel_info[1];
    uint64_t bytes = stub_kernel_info[2] - virtual_base;
    uint64_t physical = (uint64_t)_kernel_blob;
    for (uint64_t offset = 0; offset < bytes; offset += PAGE) {
        uint64_t address = virtual_base + offset;
        uint64_t *pdpt = child(pml4, (unsigned)((address >> 39) & 511));
        uint64_t *directory = child(pdpt, (unsigned)((address >> 30) & 511));
        uint64_t *last = child(directory, (unsigned)((address >> 21) & 511));
        last[(address >> 12) & 511] = (physical + offset) | PTE_PRESENT | PTE_WRITE;
    }

    handoff.magic = BOOT_STUB_MAGIC;
    handoff.kernel_physical_base = physical;
    handoff.hhdm_offset = HHDM_OFFSET;
    stub_enter_kernel((uint64_t)pml4, stub_kernel_info[0], HHDM_OFFSET + (uint64_t)&handoff,
                      (features >> 20) & 1);
}

// ---- ACPI ----

static int checksum_ok(const uint8_t *bytes, unsigned length) {
    uint8_t sum = 0;
    for (unsigned index = 0; index < length; index++) sum = (uint8_t)(sum + bytes[index]);
    return sum == 0;
}

static uint64_t scan_for_rsdp(uint64_t start, uint64_t length) {
    for (uint64_t address = start; address + 20 <= start + length; address += 16) {
        const uint8_t *bytes = (const uint8_t *)address;
        if (bytes[0] == 'R' && bytes[1] == 'S' && bytes[2] == 'D' && bytes[3] == ' ' &&
            bytes[4] == 'P' && bytes[5] == 'T' && bytes[6] == 'R' && bytes[7] == ' ' &&
            checksum_ok(bytes, 20))
            return address;
    }
    return 0;
}

static uint64_t legacy_rsdp(void) {
    // The BIOS data area is at a fixed low address the compiler takes for a null pointer.
    uint64_t bda = 0x40e;
    __asm__("" : "+r"(bda));
    uint64_t ebda = (uint64_t)(*(const volatile uint16_t *)bda) << 4;
    uint64_t found = ebda >= 0x80000 && ebda < 0xa0000 ? scan_for_rsdp(ebda, 1024) : 0;
    return found ? found : scan_for_rsdp(0xe0000, 0x20000);
}

struct efi_guid {
    uint32_t a;
    uint16_t b, c;
    uint8_t d[8];
};

struct efi_configuration_table {
    struct efi_guid guid;
    void *table;
};

static int guid_is(const struct efi_guid *guid, uint32_t a, uint16_t b, uint16_t c, uint8_t d0,
                   uint8_t d1, uint8_t d2, uint8_t d3, uint8_t d4, uint8_t d5, uint8_t d6,
                   uint8_t d7) {
    return guid->a == a && guid->b == b && guid->c == c && guid->d[0] == d0 && guid->d[1] == d1 &&
        guid->d[2] == d2 && guid->d[3] == d3 && guid->d[4] == d4 && guid->d[5] == d5 &&
        guid->d[6] == d6 && guid->d[7] == d7;
}

static uint64_t efi_rsdp(const struct efi_configuration_table *tables_found, uint64_t count) {
    uint64_t acpi1 = 0;
    for (uint64_t index = 0; index < count; index++) {
        const struct efi_guid *guid = &tables_found[index].guid;
        if (guid_is(guid, 0x8868e871, 0xe4f1, 0x11d3, 0xbc, 0x22, 0x00, 0x80, 0xc7, 0x3c, 0x88,
                    0x81))
            return (uint64_t)tables_found[index].table;
        if (guid_is(guid, 0xeb9d2d30, 0x2d88, 0x11d3, 0x9a, 0x16, 0x00, 0x90, 0x27, 0x3f, 0xc1,
                    0x4d))
            acpi1 = (uint64_t)tables_found[index].table;
    }
    return acpi1;
}

// ---- the Linux boot protocol: boot_params ----

static uint8_t byte_at(const uint8_t *base, unsigned offset) { return base[offset]; }

static uint16_t u16_at(const uint8_t *base, unsigned offset) {
    return (uint16_t)(base[offset] | (base[offset + 1] << 8));
}

static uint32_t u32_at(const uint8_t *base, unsigned offset) {
    return (uint32_t)u16_at(base, offset) | ((uint32_t)u16_at(base, offset + 2) << 16);
}

static uint64_t u64_at(const uint8_t *base, unsigned offset) {
    return (uint64_t)u32_at(base, offset) | ((uint64_t)u32_at(base, offset + 4) << 32);
}

static void framebuffer_from(uint64_t address, uint32_t pitch, uint32_t width, uint32_t height,
                             uint32_t bits, const uint8_t masks[8]) {
    struct boot_framebuffer_info *fb = &handoff.framebuffer;
    memset(fb, 0, sizeof(*fb));
    fb->magic = TUNIX_BOOT_FB_MAGIC;
    fb->version = TUNIX_BOOT_FB_VERSION;
    fb->size = sizeof(*fb);
    fb->physical_address = address;
    fb->pitch = pitch;
    fb->width = (uint16_t)width;
    fb->height = (uint16_t)height;
    fb->bits_per_pixel = (uint8_t)bits;
    fb->red_mask_size = masks[0];
    fb->red_field_position = masks[1];
    fb->green_mask_size = masks[2];
    fb->green_field_position = masks[3];
    fb->blue_mask_size = masks[4];
    fb->blue_field_position = masks[5];
    fb->reserved_mask_size = masks[6];
    fb->reserved_field_position = masks[7];
    // The console draws 32-bit pixels only.
    handoff.has_framebuffer = address && pitch && width && height && bits == 32;
}

__attribute__((noreturn)) void stub_linux_main(const uint8_t *params);

__attribute__((noreturn)) void stub_linux_main(const uint8_t *params) {
    // e820: usable RAM, and ACPI's tables and NVS, which are RAM the kernel must keep.
    unsigned entries = byte_at(params, 0x1e8);
    if (entries > 128) entries = 128;
    for (unsigned index = 0; index < entries; index++) {
        const uint8_t *entry = params + 0x2d0 + index * 20U;
        uint32_t type = u32_at(entry, 16);
        if (type == 1 || type == 3 || type == 4)
            add_region(u64_at(entry, 0), u64_at(entry, 8), type == 1);
    }

    // screen_info: a VESA linear framebuffer or the EFI GOP one, as the loader left it.
    uint8_t video = byte_at(params, 0x0f);
    if (video == 0x23 || video == 0x70) {
        uint64_t base = u32_at(params, 0x18);
        if (u32_at(params, 0x36) & 2) base |= (uint64_t)u32_at(params, 0x3a) << 32;
        uint8_t masks[8];
        for (unsigned index = 0; index < 8; index++) masks[index] = byte_at(params, 0x26 + index);
        framebuffer_from(base, u16_at(params, 0x24), u16_at(params, 0x12), u16_at(params, 0x14),
                         u16_at(params, 0x16), masks);
    }

    uint64_t command_line = u32_at(params, 0x228) | ((uint64_t)u32_at(params, 0xc8) << 32);
    if (command_line) {
        const char *text = (const char *)command_line;
        for (unsigned index = 0; index + 1 < BOOT_STUB_COMMAND_LINE && text[index]; index++)
            handoff.command_line[index] = text[index];
    }

    handoff.rsdp = u64_at(params, 0x70);
    // GRUB on UEFI says where the system table is ("EL64") rather than where the RSDP is.
    if (!handoff.rsdp && u32_at(params, 0x1c0) == 0x34364c45U) {
        uint64_t system_table = u32_at(params, 0x1c4) | ((uint64_t)u32_at(params, 0x1dc) << 32);
        if (system_table) {
            const uint8_t *table_bytes = (const uint8_t *)system_table;
            handoff.rsdp = efi_rsdp((const struct efi_configuration_table *)u64_at(table_bytes, 112),
                                    u64_at(table_bytes, 104));
        }
    }
    if (!handoff.rsdp) handoff.rsdp = legacy_rsdp();

    enter_kernel();
}

// ---- UEFI: started as an application ----

#define EFIAPI          __attribute__((ms_abi))
#define EFI_SUCCESS     0ULL
#define EFI_TOO_SMALL   0x8000000000000005ULL
#define EFI_LOADER_DATA 2U

struct efi_text_output {
    void *reset;
    uint64_t(EFIAPI *output_string)(struct efi_text_output *self, const uint16_t *text);
};

struct efi_boot_services {
    uint8_t header[24];
    void *raise_tpl, *restore_tpl, *allocate_pages, *free_pages;
    uint64_t(EFIAPI *get_memory_map)(uint64_t *size, void *map, uint64_t *key,
                                     uint64_t *descriptor_size, uint32_t *version);
    uint64_t(EFIAPI *allocate_pool)(uint32_t type, uint64_t size, void **out);
    uint64_t(EFIAPI *free_pool)(void *buffer);
    void *create_event, *set_timer, *wait_for_event, *signal_event, *close_event, *check_event;
    void *install_protocol, *reinstall_protocol, *uninstall_protocol;
    uint64_t(EFIAPI *handle_protocol)(void *handle, const struct efi_guid *guid, void **out);
    void *reserved, *register_protocol_notify, *locate_handle, *locate_device_path;
    void *install_configuration_table, *load_image, *start_image, *exit, *unload_image;
    uint64_t(EFIAPI *exit_boot_services)(void *image, uint64_t key);
    void *get_next_monotonic_count, *stall, *set_watchdog_timer, *connect_controller;
    void *disconnect_controller, *open_protocol, *close_protocol, *open_protocol_information;
    void *protocols_per_handle, *locate_handle_buffer;
    uint64_t(EFIAPI *locate_protocol)(const struct efi_guid *guid, void *registration, void **out);
};

struct efi_system_table {
    uint8_t header[24];
    void *firmware_vendor;
    uint32_t firmware_revision;
    void *console_in_handle, *console_in, *console_out_handle;
    struct efi_text_output *console_out;
    void *standard_error_handle, *standard_error, *runtime_services;
    struct efi_boot_services *boot_services;
    uint64_t table_count;
    struct efi_configuration_table *tables;
};

struct efi_loaded_image {
    uint32_t revision;
    void *parent, *system_table, *device, *file_path, *reserved;
    uint32_t options_size;
    const uint16_t *options;
    void *image_base;
    uint64_t image_size;
};

struct efi_gop_mode_info {
    uint32_t version, width, height, pixel_format;
    uint32_t red_mask, green_mask, blue_mask, reserved_mask;
    uint32_t pixels_per_line;
};

struct efi_gop_mode {
    uint32_t max_mode, mode;
    struct efi_gop_mode_info *info;
    uint64_t info_size;
    uint64_t framebuffer_base;
    uint64_t framebuffer_size;
};

struct efi_gop {
    void *query_mode, *set_mode, *blt;
    struct efi_gop_mode *mode;
};

struct efi_memory_descriptor {
    uint32_t type;
    uint32_t padding;
    uint64_t physical_start;
    uint64_t virtual_start;
    uint64_t pages;
    uint64_t attributes;
};

static struct efi_text_output *efi_console;

static void say_on_efi_console(const char *text) {
    uint16_t wide[160];
    unsigned used = 0;
    const char *prefix = "vmlinuz: ";
    for (; *prefix && used < 150; prefix++) wide[used++] = (uint8_t)*prefix;
    for (; *text && used < 156; text++) wide[used++] = (uint8_t)*text;
    wide[used++] = '\r';
    wide[used++] = '\n';
    wide[used] = 0;
    if (efi_console) efi_console->output_string(efi_console, wide);
}

static void mask_shape(uint32_t mask, uint8_t *size, uint8_t *position) {
    *size = 0;
    *position = 0;
    if (!mask) return;
    while (!(mask & 1U)) {
        mask >>= 1;
        (*position)++;
    }
    while (mask & 1U) {
        mask >>= 1;
        (*size)++;
    }
}

static void efi_framebuffer(struct efi_boot_services *services) {
    struct efi_guid gop_guid = {
        0x9042a9de, 0x23dc, 0x4a38, {0x96, 0xfb, 0x7a, 0xde, 0xd0, 0x80, 0x51, 0x6a}};
    struct efi_gop *gop = NULL;
    if (services->locate_protocol(&gop_guid, NULL, (void **)&gop) != EFI_SUCCESS || !gop ||
        !gop->mode || !gop->mode->info)
        return;
    const struct efi_gop_mode_info *info = gop->mode->info;
    uint8_t masks[8] = {8, 0, 8, 8, 8, 16, 8, 24};
    if (info->pixel_format == 1) {                  // blue, green, red, reserved
        masks[1] = 16;
        masks[5] = 0;
    } else if (info->pixel_format == 2) {           // described by masks
        mask_shape(info->red_mask, &masks[0], &masks[1]);
        mask_shape(info->green_mask, &masks[2], &masks[3]);
        mask_shape(info->blue_mask, &masks[4], &masks[5]);
        mask_shape(info->reserved_mask, &masks[6], &masks[7]);
    } else if (info->pixel_format != 0) {
        return;                                     // Blt only: no framebuffer to draw on
    }
    framebuffer_from(gop->mode->framebuffer_base, info->pixels_per_line * 4U, info->width,
                     info->height, 32, masks);
}

uint64_t EFIAPI efi_main(void *image, struct efi_system_table *system);

uint64_t EFIAPI efi_main(void *image, struct efi_system_table *system) {
    struct efi_boot_services *services = system->boot_services;
    efi_console = system->console_out;
    efi_say = say_on_efi_console;

    struct efi_guid loaded_image_guid = {
        0x5b1b31a1, 0x9562, 0x11d2, {0x8e, 0x3f, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b}};
    struct efi_loaded_image *loaded = NULL;
    if (services->handle_protocol(image, &loaded_image_guid, (void **)&loaded) == EFI_SUCCESS &&
        loaded && loaded->options) {
        unsigned characters = loaded->options_size / 2U;
        unsigned used = 0;
        for (unsigned index = 0; index < characters && used + 1 < BOOT_STUB_COMMAND_LINE; index++) {
            uint16_t character = loaded->options[index];
            if (!character) break;
            handoff.command_line[used++] = character < 0x80 ? (char)character : '?';
        }
    }

    efi_framebuffer(services);
    if (!handoff.has_framebuffer) stop("the firmware has no graphics output to draw on");
    handoff.rsdp = efi_rsdp(system->tables, system->table_count);

    // The memory map, then out of boot services; the map can change in between, and
    // then the key is stale and it has to be read again.
    uint8_t *map = NULL;
    uint64_t size = 0, key = 0, descriptor_size = 0;
    uint32_t version = 0;
    int exited = 0;
    for (unsigned attempt = 0; attempt < 4 && !exited; attempt++) {
        size = 0;
        if (services->get_memory_map(&size, NULL, &key, &descriptor_size, &version) !=
            EFI_TOO_SMALL)
            stop("the firmware did not give its memory map");
        size += 8 * descriptor_size;
        if (map) services->free_pool(map);
        map = NULL;
        if (services->allocate_pool(EFI_LOADER_DATA, size, (void **)&map) != EFI_SUCCESS)
            stop("no memory for the memory map");
        if (services->get_memory_map(&size, map, &key, &descriptor_size, &version) != EFI_SUCCESS)
            continue;
        exited = services->exit_boot_services(image, key) == EFI_SUCCESS;
    }
    if (!exited) stop("the firmware would not let go of the machine");
    efi_say = NULL;

    // Boot services' memory is the kernel's now; the loader's (this image) and the
    // firmware's runtime and ACPI memory are not.
    for (uint64_t offset = 0; offset + descriptor_size <= size; offset += descriptor_size) {
        const struct efi_memory_descriptor *descriptor =
            (const struct efi_memory_descriptor *)(map + offset);
        uint64_t length = descriptor->pages * PAGE;
        switch (descriptor->type) {
        case 3: case 4: case 7:
            add_region(descriptor->physical_start, length, 1);
            break;
        case 1: case 2: case 9: case 10:
            add_region(descriptor->physical_start, length, 0);
            break;
        default:
            break;
        }
    }
    enter_kernel();
}
