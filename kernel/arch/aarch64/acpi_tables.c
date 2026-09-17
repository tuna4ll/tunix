#include <stddef.h>
#include <stdint.h>

#include "../../include/serial.h"
#include "aarch64.h"

extern void kprintf(const char *fmt, ...);

#define HEADER_BYTES 36U
#define MAX_TABLES 64U

#define MADT_GICC 0x0BU
#define MADT_GICD 0x0CU
#define MADT_GICR 0x0EU
#define MADT_ITS 0x0FU
#define GICC_ENABLED 1U
#define GICC_ONLINE_CAPABLE 8U
#define GICR_FRAME_BYTES 0x20000ULL

#define SPCR_16550 0x00U
#define SPCR_16550_SUBSET 0x01U
#define SPCR_PL011 0x03U
#define SPCR_BCM2835 0x10U
#define SPCR_16550_GAS 0x12U

#define IORT_ITS_GROUP 0U
#define IORT_SMMU 3U
#define IORT_SMMU_V3 4U
#define IORT_ROOT_COMPLEX 2U

#define FADT_ARM_BOOT_FLAGS 129U
#define FADT_PSCI_COMPLIANT 1U
#define FADT_PSCI_USE_HVC 2U

static uint16_t u16_at(const uint8_t *bytes) {
    return (uint16_t)(bytes[0] | (bytes[1] << 8));
}

static uint32_t u32_at(const uint8_t *bytes) {
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) | ((uint32_t)bytes[2] << 16) |
           ((uint32_t)bytes[3] << 24);
}

static uint64_t u64_at(const uint8_t *bytes) {
    return (uint64_t)u32_at(bytes) | ((uint64_t)u32_at(bytes + 4) << 32);
}

static const uint8_t *table_at(uint64_t physical, const char *signature) {
    const uint8_t *header = aarch64_physical_bytes(physical, HEADER_BYTES);
    if (!header) return NULL;
    if (signature) {
        for (unsigned index = 0; index < 4U; index++)
            if (header[index] != (uint8_t)signature[index]) return NULL;
    }
    uint32_t length = u32_at(header + 4);
    if (length < HEADER_BYTES) return NULL;
    const uint8_t *table = aarch64_physical_bytes(physical, length);
    if (!table) return NULL;
    uint8_t sum = 0;
    for (uint32_t index = 0; index < length; index++) sum = (uint8_t)(sum + table[index]);
    return sum == 0 ? table : NULL;
}

static const uint8_t *find_table(const char *signature) {
    const uint8_t *rsdp = aarch64_physical_bytes(aarch64_platform.rsdp, 36U);
    if (!rsdp || rsdp[0] != 'R' || rsdp[1] != 'S' || rsdp[2] != 'D' || rsdp[3] != ' ') return NULL;
    int wide = rsdp[15] >= 2U && u64_at(rsdp + 24);
    const uint8_t *root = wide ? table_at(u64_at(rsdp + 24), "XSDT")
                               : table_at(u32_at(rsdp + 16), "RSDT");
    if (!root) return NULL;
    unsigned entry_bytes = wide ? 8U : 4U;
    uint32_t count = (u32_at(root + 4) - HEADER_BYTES) / entry_bytes;
    if (count > MAX_TABLES) count = MAX_TABLES;
    for (uint32_t index = 0; index < count; index++) {
        const uint8_t *entry = root + HEADER_BYTES + index * entry_bytes;
        uint64_t physical = wide ? u64_at(entry) : u32_at(entry);
        const uint8_t *table = table_at(physical, signature);
        if (table) return table;
    }
    return NULL;
}

static void parse_madt(const uint8_t *madt) {
    uint32_t length = u32_at(madt + 4);
    uint64_t lowest_gicr = 0, highest_gicr = 0;
    int gicr_region = 0;
    for (uint32_t offset = 44U; offset + 2U <= length;) {
        const uint8_t *entry = madt + offset;
        uint8_t type = entry[0], size = entry[1];
        if (size < 2U || offset + size > length) break;
        if (type == MADT_GICC && size >= 76U) {
            uint32_t flags = u32_at(entry + 12);
            uint64_t gicr = u64_at(entry + 60);
            if ((flags & (GICC_ENABLED | GICC_ONLINE_CAPABLE)) &&
                aarch64_platform.cpu_count < AARCH64_MAX_CPUS) {
                struct aarch64_cpu *cpu = &aarch64_platform.cpus[aarch64_platform.cpu_count++];
                cpu->mpidr = u64_at(entry + 68) & 0xFF00FFFFFFULL;
                cpu->release_address = 0;
            }
            if (!aarch64_platform.gic_cpu_interface) aarch64_platform.gic_cpu_interface = u64_at(entry + 32);
            if (gicr) {
                if (!lowest_gicr || gicr < lowest_gicr) lowest_gicr = gicr;
                if (gicr + GICR_FRAME_BYTES > highest_gicr) highest_gicr = gicr + GICR_FRAME_BYTES;
            }
        } else if (type == MADT_GICD && size >= 24U) {
            aarch64_platform.gic_distributor = u64_at(entry + 8);
            aarch64_platform.gic_version = entry[20];
        } else if (type == MADT_GICR && size >= 16U && !gicr_region) {
            aarch64_platform.gic_redistributor = u64_at(entry + 4);
            aarch64_platform.gic_redistributor_size = u32_at(entry + 12);
            gicr_region = 1;
        } else if (type == MADT_ITS && size >= 20U && !aarch64_platform.gic_its) {
            aarch64_platform.gic_its = u64_at(entry + 8);
        }
        offset += size;
    }
    if (!gicr_region && lowest_gicr) {
        aarch64_platform.gic_redistributor = lowest_gicr;
        aarch64_platform.gic_redistributor_size = highest_gicr - lowest_gicr;
    }
    if (!aarch64_platform.gic_version || aarch64_platform.gic_version > 4)
        aarch64_platform.gic_version = aarch64_platform.gic_redistributor ? 3 : 2;
    if (aarch64_platform.gic_version == 4) aarch64_platform.gic_version = 3;
}

static void parse_gtdt(const uint8_t *gtdt) {
    if (u32_at(gtdt + 4) < 72U) return;
    uint32_t virtual_timer = u32_at(gtdt + 64);
    if (virtual_timer) aarch64_platform.timer_interrupt = virtual_timer;
}

static void parse_spcr(const uint8_t *spcr) {
    if (u32_at(spcr + 4) < 80U) return;
    uint8_t interface = spcr[36];
    uint8_t space = spcr[40];
    uint8_t access = spcr[43];
    uint64_t base = u64_at(spcr + 44);
    if (space != 0U || !base) return;
    uint64_t mapped = aarch64_early_map_device(base, 0x1000ULL);
    if (!mapped) return;
    if (interface == SPCR_PL011) {
        serial_attach_pl011(mapped);
    } else if (interface == SPCR_BCM2835) {
        serial_attach_ns16550(mapped, 2, 4);
    } else if (interface == SPCR_16550 || interface == SPCR_16550_SUBSET ||
               interface == SPCR_16550_GAS) {
        unsigned width = access == 3U ? 4U : access == 2U ? 2U : 1U;
        serial_attach_ns16550(mapped, width == 4U ? 2U : width == 2U ? 1U : 0U, width);
    } else {
        return;
    }
    serial_init();
}

static void parse_mcfg(const uint8_t *mcfg) {
    uint32_t length = u32_at(mcfg + 4);
    for (uint32_t offset = 44U; offset + 16U <= length; offset += 16U) {
        const uint8_t *entry = mcfg + offset;
        if (u16_at(entry + 8) != 0U) continue;
        aarch64_platform.ecam_physical = u64_at(entry);
        aarch64_platform.ecam_first_bus = entry[10];
        aarch64_platform.ecam_last_bus = entry[11];
        aarch64_platform.ecam_size = ((uint64_t)entry[11] - entry[10] + 1U) << 20;
        return;
    }
}

static int iort_follow(const uint8_t *iort, uint32_t length, uint32_t node_offset,
                       uint32_t input, uint32_t *output, unsigned depth) {
    if (depth > 3U || node_offset + 16U > length) return -1;
    const uint8_t *node = iort + node_offset;
    if (node[0] == IORT_ITS_GROUP) {
        *output = input;
        return 0;
    }
    uint32_t mappings = u32_at(node + 8);
    uint32_t array = u32_at(node + 12);
    for (uint32_t index = 0; index < mappings; index++) {
        uint32_t at = node_offset + array + index * 20U;
        if (at + 20U > length) return -1;
        const uint8_t *map = iort + at;
        uint32_t base = u32_at(map), count = u32_at(map + 4);
        if (input < base || input - base > count) continue;
        return iort_follow(iort, length, u32_at(map + 12), u32_at(map + 8) + (input - base),
                           output, depth + 1U);
    }
    return -1;
}

static void parse_iort(const uint8_t *iort) {
    uint32_t length = u32_at(iort + 4);
    if (length < 48U) return;
    uint32_t nodes = u32_at(iort + 36);
    uint32_t offset = u32_at(iort + 40);
    for (uint32_t index = 0; index < nodes && offset + 16U <= length; index++) {
        const uint8_t *node = iort + offset;
        uint16_t size = u16_at(node + 1);
        if (size < 16U) return;
        if (node[0] == IORT_ROOT_COMPLEX && size >= 36U && u32_at(node + 28) == 0U) {
            uint32_t mappings = u32_at(node + 8);
            uint32_t array = u32_at(node + 12);
            for (uint32_t entry = 0; entry < mappings; entry++) {
                uint32_t at = offset + array + entry * 20U;
                if (at + 20U > length) break;
                const uint8_t *map = iort + at;
                uint32_t device;
                if (iort_follow(iort, length, u32_at(map + 12), u32_at(map + 8), &device, 1U) != 0)
                    continue;
                aarch64_platform.msi_rid_base = u32_at(map);
                aarch64_platform.msi_device_base = device;
                return;
            }
        }
        offset += size;
    }
}

static void parse_fadt(const uint8_t *fadt) {
    if (u32_at(fadt + 4) < FADT_ARM_BOOT_FLAGS + 2U) return;
    uint16_t flags = u16_at(fadt + FADT_ARM_BOOT_FLAGS);
    if (!(flags & FADT_PSCI_COMPLIANT)) return;
    aarch64_platform.psci_method = (flags & FADT_PSCI_USE_HVC) ? PSCI_HVC : PSCI_SMC;
}

static const uint8_t *aml_find(const uint8_t *from, const uint8_t *end, const char *text,
                               uint32_t bytes) {
    for (const uint8_t *cursor = from; cursor + bytes <= end; cursor++) {
        uint32_t index = 0;
        while (index < bytes && cursor[index] == (uint8_t)text[index]) index++;
        if (index == bytes) return cursor;
    }
    return NULL;
}

static void find_rtc(const uint8_t *fadt) {
    uint32_t fadt_length = u32_at(fadt + 4);
    uint64_t dsdt_physical = fadt_length >= 148U ? u64_at(fadt + 140) : 0;
    if (!dsdt_physical) dsdt_physical = u32_at(fadt + 40);
    const uint8_t *dsdt = table_at(dsdt_physical, "DSDT");
    if (!dsdt) return;
    const uint8_t *end = dsdt + u32_at(dsdt + 4);
    const uint8_t *hid = aml_find(dsdt + HEADER_BYTES, end, "ARMH0061", 8U);
    if (!hid) return;
    const uint8_t *limit = hid + 128 < end ? hid + 128 : end;
    for (const uint8_t *cursor = hid; cursor + 12 <= limit; cursor++) {
        if (cursor[0] != 0x86U || cursor[1] != 0x09U || cursor[2] != 0x00U) continue;
        uint32_t base = u32_at(cursor + 4);
        if (base) aarch64_platform.rtc_base = aarch64_early_map_device(base, 0x1000ULL);
        return;
    }
}

int aarch64_acpi_discover(void) {
    const uint8_t *madt = find_table("APIC");
    if (!madt) return -1;
    aarch64_platform.cpu_count = 0;
    parse_madt(madt);

    const uint8_t *spcr = find_table("SPCR");
    if (spcr) parse_spcr(spcr);
    const uint8_t *gtdt = find_table("GTDT");
    if (gtdt) parse_gtdt(gtdt);
    const uint8_t *mcfg = find_table("MCFG");
    if (mcfg) parse_mcfg(mcfg);
    const uint8_t *iort = find_table("IORT");
    if (iort) parse_iort(iort);
    const uint8_t *fadt = find_table("FACP");
    if (fadt) {
        parse_fadt(fadt);
        find_rtc(fadt);
    }

    kprintf("ACPI: GICv%d at %p, %u cpu(s), timer %u, ECAM %p, ITS %p, PSCI %s\n",
            aarch64_platform.gic_version, (void *)aarch64_platform.gic_distributor,
            aarch64_platform.cpu_count, aarch64_platform.timer_interrupt,
            (void *)aarch64_platform.ecam_physical, (void *)aarch64_platform.gic_its,
            aarch64_platform.psci_method == PSCI_HVC   ? "hvc"
            : aarch64_platform.psci_method == PSCI_SMC ? "smc"
                                                       : "none");
    return 0;
}
