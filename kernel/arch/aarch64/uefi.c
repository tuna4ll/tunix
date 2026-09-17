#include <stddef.h>
#include <stdint.h>

#include "../../include/boot_framebuffer.h"
#include "../../include/pmm.h"
#include "../../include/time.h"
#include "../../include/vmm.h"
#include "aarch64.h"

extern void kprintf(const char *fmt, ...);

#define DESC_SH_INNER (3ULL << 8)
#define DESC_AF (1ULL << 10)
#define DESC_PXN (1ULL << 53)
#define DESC_UXN (1ULL << 54)
#define BLOCK_2M 0x200000ULL
#define EFI_PAGE_BYTES 4096ULL

#define EFI_RESERVED 0U
#define EFI_LOADER_CODE 1U
#define EFI_LOADER_DATA 2U
#define EFI_BOOT_SERVICES_CODE 3U
#define EFI_BOOT_SERVICES_DATA 4U
#define EFI_RUNTIME_SERVICES_CODE 5U
#define EFI_RUNTIME_SERVICES_DATA 6U
#define EFI_CONVENTIONAL 7U
#define EFI_ACPI_RECLAIM 9U
#define EFI_ACPI_NVS 10U
#define EFI_PERSISTENT 14U

#define EFI_MMIO 11U
#define EFI_MMIO_PORT 12U
#define EFI_MEMORY_RUNTIME (1ULL << 63)
#define DESC_ATTR_DEVICE (1ULL << 2)
#define SYSTEM_TABLE_RUNTIME 88U
#define RUNTIME_GET_TIME 24U
#define EFI_UNSPECIFIED_TIMEZONE 0x07FF
#define SYSTEM_TABLE_ENTRY_COUNT 104U
#define SYSTEM_TABLE_ENTRIES 112U
#define CONFIG_ENTRY_BYTES 24U
#define SCREEN_INFO_BYTES 64U
#define VIDEO_TYPE_EFI 0x70U
#define VIDEO_CAPABILITY_64BIT_BASE 2U

struct efi_guid_bytes {
    uint8_t bytes[16];
};

static const struct efi_guid_bytes acpi20_guid = {{
    0x71, 0xe8, 0x68, 0x88, 0xf1, 0xe4, 0xd3, 0x11,
    0xbc, 0x22, 0x00, 0x80, 0xc7, 0x3c, 0x88, 0x81,
}};
static const struct efi_guid_bytes acpi10_guid = {{
    0x30, 0x2d, 0x9d, 0xeb, 0x88, 0x2d, 0xd3, 0x11,
    0x9a, 0x16, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0x4d,
}};
static const struct efi_guid_bytes screen_info_guid = {{
    0x0a, 0xc2, 0x3f, 0xe0, 0xdc, 0x85, 0x6e, 0x40,
    0xb9, 0x0e, 0x4a, 0xb5, 0x02, 0x37, 0x1d, 0x95,
}};

static struct boot_framebuffer_info framebuffer;

static uint64_t chosen_u64(const struct fdt_node *chosen, const char *name) {
    uint32_t length = 0;
    const uint8_t *value = fdt_property(chosen, name, &length);
    if (!value) return 0;
    if (length == 8U) return ((uint64_t)fdt_read32(value) << 32) | fdt_read32(value + 4);
    if (length == 4U) return fdt_read32(value);
    return 0;
}

int uefi_detect(void) {
    struct fdt_node chosen;
    if (fdt_find_path("/chosen", &chosen) != 0) return 0;
    uint64_t table = chosen_u64(&chosen, "linux,uefi-system-table");
    uint64_t map = chosen_u64(&chosen, "linux,uefi-mmap-start");
    uint64_t size = chosen_u64(&chosen, "linux,uefi-mmap-size");
    uint64_t descriptor = chosen_u64(&chosen, "linux,uefi-mmap-desc-size");
    if (!table || !map || !size || descriptor < 40U) return 0;
    aarch64_platform.uefi_system_table = table;
    aarch64_platform.uefi_map = map;
    aarch64_platform.uefi_map_size = size;
    aarch64_platform.uefi_descriptor_size = descriptor;

    uint64_t attributes = DESC_AF | DESC_SH_INNER | DESC_PXN | DESC_UXN;
    for (uint64_t page = map & ~0xFFFULL; page < map + size; page += EFI_PAGE_BYTES)
        if (aarch64_early_map(page, page, attributes, 3) != 0) return 0;
    return 1;
}

static int descriptor_at(uint64_t index, uint32_t *type, uint64_t *base, uint64_t *bytes,
                         uint64_t *virtual_base);

static uint64_t descriptor_attributes(uint64_t index) {
    uint64_t offset = index * aarch64_platform.uefi_descriptor_size;
    uint64_t value;
    __builtin_memcpy(&value, (const uint8_t *)(aarch64_platform.uefi_map + offset + 32U), 8);
    return value;
}

static int descriptor_at(uint64_t index, uint32_t *type, uint64_t *base, uint64_t *bytes,
                         uint64_t *virtual_base) {
    uint64_t offset = index * aarch64_platform.uefi_descriptor_size;
    if (offset + 40U > aarch64_platform.uefi_map_size) return -1;
    const uint8_t *entry = (const uint8_t *)(aarch64_platform.uefi_map + offset);
    uint32_t kind;
    uint64_t physical, virtual_address, pages;
    __builtin_memcpy(&kind, entry, 4);
    __builtin_memcpy(&physical, entry + 8, 8);
    __builtin_memcpy(&virtual_address, entry + 16, 8);
    __builtin_memcpy(&pages, entry + 24, 8);
    *type = kind;
    *base = physical;
    *bytes = pages * EFI_PAGE_BYTES;
    if (virtual_base) *virtual_base = virtual_address;
    return 0;
}

static int type_is_ram(uint32_t type) {
    return (type >= EFI_LOADER_CODE && type <= EFI_CONVENTIONAL) ||
           type == EFI_ACPI_RECLAIM || type == EFI_ACPI_NVS || type == EFI_PERSISTENT;
}

static int type_is_usable(uint32_t type) {
    return type == EFI_LOADER_CODE || type == EFI_LOADER_DATA ||
           type == EFI_BOOT_SERVICES_CODE || type == EFI_BOOT_SERVICES_DATA ||
           type == EFI_CONVENTIONAL;
}

void uefi_collect_ram(void) {
    uint32_t type;
    uint64_t base, bytes;
    for (uint64_t index = 0; descriptor_at(index, &type, &base, &bytes, NULL) == 0; index++) {
        if (!bytes || !type_is_ram(type)) continue;
        struct aarch64_range *last = aarch64_platform.ram_count
                                         ? &aarch64_platform.ram[aarch64_platform.ram_count - 1U]
                                         : NULL;
        if (last && last->base + last->size == base) {
            last->size += bytes;
            continue;
        }
        if (aarch64_platform.ram_count >= AARCH64_RAM_RANGES) continue;
        aarch64_platform.ram[aarch64_platform.ram_count].base = base;
        aarch64_platform.ram[aarch64_platform.ram_count].size = bytes;
        aarch64_platform.ram_count++;
    }
}

void uefi_each_region(void (*visit)(uint64_t base, uint64_t end, int usable)) {
    uint32_t type;
    uint64_t base, bytes;
    uint64_t run_base = 0, run_end = 0;
    int run_usable = -1;
    for (uint64_t index = 0; descriptor_at(index, &type, &base, &bytes, NULL) == 0; index++) {
        if (!bytes || !type_is_ram(type)) continue;
        int usable = type_is_usable(type);
        if (run_usable == usable && run_end == base) {
            run_end = base + bytes;
            continue;
        }
        if (run_usable >= 0) visit(run_base, run_end, run_usable);
        run_base = base;
        run_end = base + bytes;
        run_usable = usable;
    }
    if (run_usable >= 0) visit(run_base, run_end, run_usable);
}

static uint64_t firmware_physical(uint64_t address) {
    uint32_t type;
    uint64_t base, bytes, virtual_base;
    for (uint64_t index = 0; descriptor_at(index, &type, &base, &bytes, &virtual_base) == 0; index++) {
        if (!virtual_base || virtual_base == base) continue;
        if (address >= virtual_base && address - virtual_base < bytes)
            return base + (address - virtual_base);
    }
    return address;
}

const uint8_t *aarch64_physical_bytes(uint64_t physical, uint64_t bytes) {
    if (!physical || physical + bytes > PMM_DIRECT_MAP_LIMIT || physical + bytes < physical)
        return NULL;
    uint64_t first = physical >> 30;
    uint64_t last = (physical + bytes - 1U) >> 30;
    for (uint64_t gigabyte = first; gigabyte <= last; gigabyte++) {
        int mapped = 0;
        for (unsigned index = 0; index < aarch64_platform.ram_count && !mapped; index++) {
            const struct aarch64_range *range = &aarch64_platform.ram[index];
            mapped = (range->base >> 30) <= gigabyte &&
                     ((range->base + range->size - 1U) >> 30) >= gigabyte;
        }
        if (!mapped) return NULL;
    }
    return (const uint8_t *)(DIRECT_MAP_BASE + physical);
}

static uint64_t read_u64(const uint8_t *bytes) {
    uint64_t value;
    __builtin_memcpy(&value, bytes, 8);
    return value;
}

static void use_screen_info(const uint8_t *info) {
    if (info[15] != VIDEO_TYPE_EFI) return;
    uint16_t width = (uint16_t)(info[18] | (info[19] << 8));
    uint16_t height = (uint16_t)(info[20] | (info[21] << 8));
    uint16_t depth = (uint16_t)(info[22] | (info[23] << 8));
    uint64_t base = (uint64_t)info[24] | ((uint64_t)info[25] << 8) |
                    ((uint64_t)info[26] << 16) | ((uint64_t)info[27] << 24);
    uint32_t capabilities = (uint32_t)info[54] | ((uint32_t)info[55] << 8) |
                            ((uint32_t)info[56] << 16) | ((uint32_t)info[57] << 24);
    if (capabilities & VIDEO_CAPABILITY_64BIT_BASE)
        base |= ((uint64_t)info[58] | ((uint64_t)info[59] << 8) |
                 ((uint64_t)info[60] << 16) | ((uint64_t)info[61] << 24)) << 32;
    uint16_t line = (uint16_t)(info[36] | (info[37] << 8));
    if (!base || !width || !height || depth != 32U || line < width * 4U) return;

    framebuffer.magic = TUNIX_BOOT_FB_MAGIC;
    framebuffer.version = TUNIX_BOOT_FB_VERSION;
    framebuffer.size = sizeof(framebuffer);
    framebuffer.physical_address = base;
    framebuffer.pitch = line;
    framebuffer.width = width;
    framebuffer.height = height;
    framebuffer.bits_per_pixel = 32;
    framebuffer.red_mask_size = info[38];
    framebuffer.red_field_position = info[39];
    framebuffer.green_mask_size = info[40];
    framebuffer.green_field_position = info[41];
    framebuffer.blue_mask_size = info[42];
    framebuffer.blue_field_position = info[43];

    uint64_t first = base & ~(BLOCK_2M - 1U);
    uint64_t end = (base + (uint64_t)line * height + BLOCK_2M - 1U) & ~(BLOCK_2M - 1U);
    for (unsigned index = 0; index < aarch64_platform.ram_count; index++) {
        const struct aarch64_range *range = &aarch64_platform.ram[index];
        if (end <= range->base || first >= range->base + range->size) continue;
        aarch64_platform.display_hole_base = first;
        aarch64_platform.display_hole_size = end - first;
    }
}

static int guid_equal(const uint8_t *entry, const struct efi_guid_bytes *guid) {
    for (unsigned index = 0; index < 16U; index++)
        if (entry[index] != guid->bytes[index]) return 0;
    return 1;
}

void uefi_scan_tables(void) {
    const uint8_t *system = aarch64_physical_bytes(aarch64_platform.uefi_system_table,
                                           SYSTEM_TABLE_ENTRIES + 8U);
    if (!system) {
        kprintf("UEFI: system table at %p is not in memory\n",
                (void *)aarch64_platform.uefi_system_table);
        return;
    }
    uint64_t count = read_u64(system + SYSTEM_TABLE_ENTRY_COUNT);
    uint64_t tables = firmware_physical(read_u64(system + SYSTEM_TABLE_ENTRIES));
    if (count > 256U) count = 256U;
    const uint8_t *entries = aarch64_physical_bytes(tables, count * CONFIG_ENTRY_BYTES);
    if (!entries) return;

    uint64_t acpi10 = 0;
    for (uint64_t index = 0; index < count; index++) {
        const uint8_t *entry = entries + index * CONFIG_ENTRY_BYTES;
        uint64_t pointer = firmware_physical(read_u64(entry + 16));
        if (guid_equal(entry, &acpi20_guid)) {
            aarch64_platform.rsdp = pointer;
        } else if (guid_equal(entry, &acpi10_guid)) {
            acpi10 = pointer;
        } else if (guid_equal(entry, &screen_info_guid)) {
            const uint8_t *info = aarch64_physical_bytes(pointer, SCREEN_INFO_BYTES);
            if (info) use_screen_info(info);
        }
    }
    if (!aarch64_platform.rsdp) aarch64_platform.rsdp = acpi10;
}

const struct boot_framebuffer_info *uefi_framebuffer(void) {
    return framebuffer.magic ? &framebuffer : NULL;
}

struct efi_time {
    uint16_t year;
    uint8_t month;
    uint8_t day;
    uint8_t hour;
    uint8_t minute;
    uint8_t second;
    uint8_t pad1;
    uint32_t nanosecond;
    int16_t timezone;
    uint8_t daylight;
    uint8_t pad2;
};

typedef uint64_t (*efi_get_time_fn)(struct efi_time *time, void *capabilities);

static int map_runtime(void) {
    uint32_t type;
    uint64_t base, bytes, virtual_base;
    for (uint64_t index = 0; descriptor_at(index, &type, &base, &bytes, &virtual_base) == 0; index++) {
        if (!(descriptor_attributes(index) & EFI_MEMORY_RUNTIME)) continue;
        uint64_t target = virtual_base ? virtual_base : base;
        uint64_t attributes = DESC_AF | DESC_UXN;
        if (type == EFI_MMIO || type == EFI_MMIO_PORT) attributes |= DESC_ATTR_DEVICE | DESC_PXN;
        else if (type == EFI_RUNTIME_SERVICES_CODE) attributes |= DESC_SH_INNER;
        else attributes |= DESC_SH_INNER | DESC_PXN;
        for (uint64_t offset = 0; offset < bytes;) {
            int block = !((target + offset) & (BLOCK_2M - 1U)) &&
                        !((base + offset) & (BLOCK_2M - 1U)) && bytes - offset >= BLOCK_2M;
            if (aarch64_early_map(target + offset, base + offset, attributes, block ? 2 : 3) != 0)
                return -1;
            offset += block ? BLOCK_2M : EFI_PAGE_BYTES;
        }
    }
    return 0;
}

void uefi_read_time(void) {
    kprintf("UEFI: system table at %p, ACPI at %p, framebuffer %ux%u at %p\n",
            (void *)aarch64_platform.uefi_system_table, (void *)aarch64_platform.rsdp,
            framebuffer.width, framebuffer.height, (void *)framebuffer.physical_address);
    const uint8_t *system = aarch64_physical_bytes(aarch64_platform.uefi_system_table,
                                                   SYSTEM_TABLE_ENTRIES + 8U);
    if (!system) return;
    uint64_t runtime_virtual = read_u64(system + SYSTEM_TABLE_RUNTIME);
    const uint8_t *runtime = aarch64_physical_bytes(firmware_physical(runtime_virtual),
                                                    RUNTIME_GET_TIME + 8U);
    if (!runtime) return;
    uint64_t get_time = read_u64(runtime + RUNTIME_GET_TIME);
    if (!get_time || map_runtime() != 0) {
        kprintf("UEFI: runtime services could not be mapped\n");
        return;
    }

    struct efi_time now;
    __builtin_memset(&now, 0, sizeof(now));
    uint64_t counter;
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(counter));
    uint64_t status = ((efi_get_time_fn)get_time)(&now, NULL);
    if (status != 0 || now.year < 1970U || !now.month || now.month > 12U || !now.day) {
        kprintf("UEFI: GetTime failed with status %p\n", (void *)status);
        return;
    }
    struct tunix_rtc_time calendar = {
        .year = now.year, .month = now.month, .day = now.day,
        .hour = now.hour, .minute = now.minute, .second = now.second,
    };
    int64_t seconds = (int64_t)time_calendar_to_epoch(&calendar);
    if (now.timezone != EFI_UNSPECIFIED_TIMEZONE) seconds += (int64_t)now.timezone * 60;
    aarch64_platform.firmware_epoch = (uint64_t)seconds;
    aarch64_platform.firmware_epoch_counter = counter;
    kprintf("UEFI: time %u-%u-%u %u:%u:%u\n", now.year, now.month, now.day, now.hour,
            now.minute, now.second);
}
