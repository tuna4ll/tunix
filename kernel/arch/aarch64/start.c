#include <stddef.h>
#include <stdint.h>

#include "../../include/boot.h"
#include "../../include/kstring.h"
#include "../../include/percpu.h"
#include "../../include/pmm.h"
#include "../../include/sdhci.h"
#include "../../include/serial.h"
#include "../../include/vmm.h"
#include "aarch64.h"

#define DESC_SH_INNER (3ULL << 8)
#define DESC_AF (1ULL << 10)
#define DESC_PXN (1ULL << 53)
#define DESC_UXN (1ULL << 54)
#define RESERVED_RANGES 32U
#define COMMAND_LINE_BYTES 1024U

extern char kernel_image_start[];
extern char __image_end[];
extern void kmain(const struct boot_info *boot);
extern void kprintf(const char *fmt, ...);

struct aarch64_platform aarch64_platform;

static struct boot_info info;
static struct boot_memory_region regions[BOOT_MEMORY_REGIONS];
static struct aarch64_range reserved[RESERVED_RANGES];
static unsigned reserved_count;
static char command_line[COMMAND_LINE_BYTES];

const struct boot_info *boot_info(void) { return &info; }

int aarch64_physical_is_ram(uint64_t physical) {
    if (aarch64_platform.display_hole_size && physical >= aarch64_platform.display_hole_base &&
        physical - aarch64_platform.display_hole_base < aarch64_platform.display_hole_size)
        return 0;
    for (unsigned index = 0; index < aarch64_platform.ram_count; index++) {
        const struct aarch64_range *range = &aarch64_platform.ram[index];
        if (physical >= range->base && physical - range->base < range->size) return 1;
    }
    return 0;
}

static int text_equal(const char *a, const char *b) {
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static void reserve(uint64_t base, uint64_t size) {
    if (!size || reserved_count >= RESERVED_RANGES) return;
    uint64_t start = base & ~0xFFFULL;
    uint64_t end = (base + size + 0xFFFULL) & ~0xFFFULL;
    reserved[reserved_count].base = start;
    reserved[reserved_count].size = end - start;
    reserved_count++;
}

static void add_region(uint64_t base, uint64_t end, uint32_t usable) {
    if (end <= base || info.memory_count >= BOOT_MEMORY_REGIONS) return;
    regions[info.memory_count].base = base;
    regions[info.memory_count].length = end - base;
    regions[info.memory_count].usable = usable;
    info.memory_count++;
}

static void add_usable_without_reserved(uint64_t base, uint64_t end, unsigned from) {
    for (unsigned index = from; index < reserved_count; index++) {
        uint64_t cut_start = reserved[index].base;
        uint64_t cut_end = cut_start + reserved[index].size;
        if (cut_end <= base || cut_start >= end) continue;
        if (cut_start > base) add_usable_without_reserved(base, cut_start, index + 1U);
        uint64_t hole_start = cut_start > base ? cut_start : base;
        uint64_t hole_end = cut_end < end ? cut_end : end;
        add_region(hole_start, hole_end, 0);
        if (cut_end < end) add_usable_without_reserved(cut_end, end, index + 1U);
        return;
    }
    add_region(base, end, 1);
}

static void collect_memory(uint64_t dtb_physical, uint64_t load_physical) {
    struct fdt_node node;
    for (unsigned index = 0; fdt_find_device_type("memory", index, &node) == 0; index++) {
        uint64_t base, size;
        for (unsigned entry = 0; fdt_reg(&node, entry, &base, &size) == 0; entry++) {
            if (!size || aarch64_platform.ram_count >= AARCH64_RAM_RANGES) continue;
            aarch64_platform.ram[aarch64_platform.ram_count].base = base;
            aarch64_platform.ram[aarch64_platform.ram_count].size = size;
            aarch64_platform.ram_count++;
        }
    }

    reserve(load_physical, (uint64_t)(__image_end - kernel_image_start));
    aarch64_display_reserve();
    reserve(aarch64_platform.display_hole_base, aarch64_platform.display_hole_size);
    reserve(dtb_physical, fdt_total_size());
    uint64_t base, size;
    for (unsigned index = 0; fdt_memreserve(index, &base, &size) == 0; index++) reserve(base, size);
    if (fdt_find_path("/reserved-memory", &node) == 0) {
        struct fdt_node child;
        for (unsigned index = 0; fdt_child(&node, index, &child) == 0; index++) {
            child.address_cells = node.address_cells;
            child.size_cells = node.size_cells;
            uint32_t length = 0;
            const void *cells = fdt_property(&node, "#address-cells", &length);
            if (cells && length == 4U) child.address_cells = fdt_read32(cells);
            cells = fdt_property(&node, "#size-cells", &length);
            if (cells && length == 4U) child.size_cells = fdt_read32(cells);
            if (fdt_reg(&child, 0, &base, &size) == 0) reserve(base, size);
        }
    }
    struct fdt_node chosen;
    if (fdt_find_path("/chosen", &chosen) == 0) {
        uint32_t length = 0;
        const void *start = fdt_property(&chosen, "linux,initrd-start", &length);
        const void *end = fdt_property(&chosen, "linux,initrd-end", NULL);
        if (start && end) {
            uint64_t first = length == 8U ? ((uint64_t)fdt_read32(start) << 32) | fdt_read32((const uint8_t *)start + 4)
                                          : fdt_read32(start);
            uint64_t last = length == 8U ? ((uint64_t)fdt_read32(end) << 32) | fdt_read32((const uint8_t *)end + 4)
                                         : fdt_read32(end);
            if (last > first) reserve(first, last - first);
        }
    }

    info.memory = regions;
    info.memory_count = 0;
    for (unsigned index = 0; index < aarch64_platform.ram_count; index++) {
        const struct aarch64_range *range = &aarch64_platform.ram[index];
        add_usable_without_reserved(range->base, range->base + range->size, 0);
    }
}

static void copy_command_line(void) {
    struct fdt_node chosen;
    command_line[0] = '\0';
    if (fdt_find_path("/chosen", &chosen) != 0) return;
    uint32_t length = 0;
    const char *arguments = fdt_property(&chosen, "bootargs", &length);
    if (!arguments) return;
    uint32_t index = 0;
    while (index + 1U < COMMAND_LINE_BYTES && index < length && arguments[index]) {
        command_line[index] = arguments[index];
        index++;
    }
    command_line[index] = '\0';
}

static int stdout_node(struct fdt_node *out) {
    struct fdt_node chosen;
    if (fdt_find_path("/chosen", &chosen) != 0) return -1;
    const char *path = fdt_property(&chosen, "stdout-path", NULL);
    if (!path) path = fdt_property(&chosen, "linux,stdout-path", NULL);
    if (!path) return -1;

    char name[128];
    unsigned length = 0;
    while (path[length] && path[length] != ':' && length + 1U < sizeof(name)) {
        name[length] = path[length];
        length++;
    }
    name[length] = '\0';
    if (name[0] == '/') return fdt_find_path(name, out);

    struct fdt_node aliases;
    if (fdt_find_path("/aliases", &aliases) != 0) return -1;
    const char *resolved = fdt_property(&aliases, name, NULL);
    return resolved ? fdt_find_path(resolved, out) : -1;
}

static uint32_t property_u32(const struct fdt_node *node, const char *name, uint32_t fallback) {
    uint32_t length = 0;
    const void *value = fdt_property(node, name, &length);
    return value && length >= 4U ? fdt_read32(value) : fallback;
}

static void attach_console(void) {
    struct fdt_node node;
    if (stdout_node(&node) != 0 &&
        fdt_find_compatible("arm,pl011", 0, &node) != 0) return;
    uint64_t base, size;
    if (fdt_reg_cpu(&node, 0, &base, &size) != 0) return;
    uint64_t mapped = aarch64_early_map_device(base, size < 0x1000ULL ? 0x1000ULL : size);
    if (!mapped) return;
    if (fdt_is_compatible(&node, "arm,pl011")) {
        serial_attach_pl011(mapped);
    } else if (fdt_is_compatible(&node, "brcm,bcm2835-aux-uart")) {
        serial_attach_ns16550(mapped, 2, 4);
    } else if (fdt_is_compatible(&node, "ns16550a") || fdt_is_compatible(&node, "ns16550") ||
               fdt_is_compatible(&node, "snps,dw-apb-uart")) {
        serial_attach_ns16550(mapped, property_u32(&node, "reg-shift", 0),
                              property_u32(&node, "reg-io-width", 1));
    } else {
        return;
    }
    serial_init();
}

static void discover_devices(void) {
    struct fdt_node node;
    uint64_t base, size;

    if (fdt_find_compatible("arm,gic-v3", 0, &node) == 0) {
        aarch64_platform.gic_version = 3;
        if (fdt_reg_cpu(&node, 0, &base, &size) == 0) aarch64_platform.gic_distributor = base;
        if (fdt_reg_cpu(&node, 1, &base, &size) == 0) {
            aarch64_platform.gic_redistributor = base;
            aarch64_platform.gic_redistributor_size = size;
        }
        if (fdt_find_compatible("arm,gic-v3-its", 0, &node) == 0 &&
            fdt_reg_cpu(&node, 0, &base, &size) == 0)
            aarch64_platform.gic_its = base;
    } else if (fdt_find_compatible("arm,gic-400", 0, &node) == 0 ||
               fdt_find_compatible("arm,cortex-a15-gic", 0, &node) == 0) {
        aarch64_platform.gic_version = 2;
        if (fdt_reg_cpu(&node, 0, &base, &size) == 0) aarch64_platform.gic_distributor = base;
        if (fdt_reg_cpu(&node, 1, &base, &size) == 0) aarch64_platform.gic_cpu_interface = base;
    }

    aarch64_platform.timer_interrupt = 27;
    if (fdt_find_compatible("arm,armv8-timer", 0, &node) == 0) {
        uint32_t length = 0;
        const uint8_t *interrupts = fdt_property(&node, "interrupts", &length);
        if (interrupts && length >= 36U) aarch64_platform.timer_interrupt = fdt_read32(interrupts + 28) + 16U;
        aarch64_platform.timer_frequency = property_u32(&node, "clock-frequency", 0);
    }

    if (fdt_find_compatible("arm,pl031", 0, &node) == 0 && fdt_reg_cpu(&node, 0, &base, &size) == 0)
        aarch64_platform.rtc_base = aarch64_early_map_device(base, 0x1000ULL);

    if (fdt_find_compatible("pci-host-ecam-generic", 0, &node) == 0 &&
        fdt_reg_cpu(&node, 0, &base, &size) == 0) {
        aarch64_platform.ecam_physical = base;
        aarch64_platform.ecam_size = size;
        uint32_t length = 0;
        const uint8_t *range = fdt_property(&node, "bus-range", &length);
        aarch64_platform.ecam_first_bus = range && length >= 8U ? fdt_read32(range) : 0;
        aarch64_platform.ecam_last_bus = range && length >= 8U ? fdt_read32(range + 4) : 255;
        const uint8_t *msi = fdt_property(&node, "msi-map", &length);
        if (msi && length >= 16U) {
            aarch64_platform.msi_rid_base = fdt_read32(msi);
            aarch64_platform.msi_device_base = fdt_read32(msi + 8);
        }
        const uint8_t *ranges = fdt_property(&node, "ranges", &length);
        uint32_t parent_cells = node.address_cells;
        uint32_t entry = (3U + parent_cells + 2U) * 4U;
        for (uint32_t offset = 0; ranges && offset + entry <= length; offset += entry) {
            uint32_t space = (fdt_read32(ranges + offset) >> 24) & 3U;
            uint64_t cpu = 0;
            for (uint32_t cell = 0; cell < parent_cells; cell++)
                cpu = (cpu << 32) | fdt_read32(ranges + offset + 12U + cell * 4U);
            uint64_t bytes = ((uint64_t)fdt_read32(ranges + offset + 12U + parent_cells * 4U) << 32) |
                             fdt_read32(ranges + offset + 16U + parent_cells * 4U);
            if (space == 2U && !aarch64_platform.pci_mmio32_size) {
                aarch64_platform.pci_mmio32_base = cpu;
                aarch64_platform.pci_mmio32_size = bytes;
            } else if (space == 3U && !aarch64_platform.pci_mmio64_size) {
                aarch64_platform.pci_mmio64_base = cpu;
                aarch64_platform.pci_mmio64_size = bytes;
            }
        }
    }

    if (fdt_find_compatible("arm,psci-1.0", 0, &node) == 0 ||
        fdt_find_compatible("arm,psci-0.2", 0, &node) == 0) {
        const char *method = fdt_property(&node, "method", NULL);
        if (method && text_equal(method, "smc")) aarch64_platform.psci_method = PSCI_SMC;
        else if (method && text_equal(method, "hvc")) aarch64_platform.psci_method = PSCI_HVC;
    }

    static const char *const sd_compatibles[] = {
        "brcm,bcm2711-emmc2", "brcm,bcm2835-sdhci", "arasan,sdhci-5.1", "arasan,sdhci-8.9a",
        "snps,dwcmshc-sdhci", "rockchip,rk3588-dwcmshc", "rockchip,rk3568-dwcmshc",
    };
    for (unsigned kind = 0; kind < sizeof(sd_compatibles) / sizeof(sd_compatibles[0]); kind++) {
        for (unsigned index = 0; fdt_find_compatible(sd_compatibles[kind], index, &node) == 0; index++) {
            const char *status = fdt_property(&node, "status", NULL);
            if (status && !text_equal(status, "okay") && !text_equal(status, "ok")) continue;
            if (aarch64_platform.sd_count >= AARCH64_MAX_SD) break;
            if (fdt_reg_cpu(&node, 0, &base, &size) != 0) continue;
            int duplicate = 0;
            for (unsigned seen = 0; seen < aarch64_platform.sd_count; seen++)
                if (aarch64_platform.sd[seen].physical == base) duplicate = 1;
            if (duplicate) continue;
            struct aarch64_sd *sd = &aarch64_platform.sd[aarch64_platform.sd_count++];
            sd->physical = base;
            sd->clock_hz = property_u32(&node, "clock-frequency", 0);
            sd->quirks = fdt_is_compatible(&node, "brcm,bcm2835-sdhci") ? SDHCI_QUIRK_WRITE_DELAY : 0;
        }
    }

    aarch64_platform.cpu_count = 0;
    while (fdt_find_device_type("cpu", aarch64_platform.cpu_count, &node) == 0) {
        unsigned index = aarch64_platform.cpu_count++;
        if (index >= AARCH64_MAX_CPUS) continue;
        if (fdt_reg(&node, 0, &base, &size) == 0) aarch64_platform.cpus[index].mpidr = base;
        const char *method = fdt_property(&node, "enable-method", NULL);
        uint32_t length = 0;
        const uint8_t *release = fdt_property(&node, "cpu-release-addr", &length);
        if (method && text_equal(method, "spin-table") && release && length == 8U)
            aarch64_platform.cpus[index].release_address =
                ((uint64_t)fdt_read32(release) << 32) | fdt_read32(release + 4);
    }
}

static void map_direct_memory(void) {
    uint64_t attributes = DESC_AF | DESC_SH_INNER | DESC_PXN | DESC_UXN;
    for (unsigned index = 0; index < aarch64_platform.ram_count; index++) {
        const struct aarch64_range *range = &aarch64_platform.ram[index];
        uint64_t first = range->base >> 30;
        uint64_t last = (range->base + range->size - 1U) >> 30;
        for (uint64_t gigabyte = first; gigabyte <= last; gigabyte++) {
            uint64_t physical = gigabyte << 30;
            if (physical >= PMM_DIRECT_MAP_LIMIT) break;
            aarch64_early_map(DIRECT_MAP_BASE + physical, physical, attributes, 1);
        }
    }
}

void aarch64_start(uint64_t dtb_physical, uint64_t load_physical) {
    aarch64_platform.load_offset = load_physical - AARCH64_KERNEL_VIRTUAL_BASE;
    percpu_activate(0);

    if (fdt_init((const void *)dtb_physical) != 0) {
        for (;;) __asm__ volatile("wfi");
    }
    attach_console();
    kprintf("\nTUNIX: aarch64 kernel at %p, loaded at %p, device tree at %p\n",
            (void *)kernel_image_start, (void *)load_physical, (void *)dtb_physical);

    discover_devices();
    collect_memory(dtb_physical, load_physical);
    copy_command_line();
    map_direct_memory();

    info.command_line = command_line;
    info.framebuffer = aarch64_display_setup();
    info.rsdp = 0;
    info.hhdm_offset = DIRECT_MAP_BASE;
    info.kernel_physical_base = load_physical;
    info.kernel_virtual_base = (uint64_t)kernel_image_start;
    info.kernel_size = (uint64_t)(__image_end - kernel_image_start);

    kprintf("TUNIX: %u memory range(s), %u region(s), GICv%d, timer %u, %u cpu(s)\n",
            aarch64_platform.ram_count, info.memory_count, aarch64_platform.gic_version,
            aarch64_platform.timer_interrupt, aarch64_platform.cpu_count);
    kmain(&info);
}
