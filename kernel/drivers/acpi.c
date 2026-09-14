#include <stdint.h>
#include <stddef.h>

#include "../include/acpi.h"
#include "../include/apic.h"
#include "../include/cpu.h"
#include "../include/io.h"
#include "../include/time.h"
#include "../include/vmm.h"
#include "../include/kstring.h"

extern void kprintf(const char *fmt, ...);
#include "../include/boot.h"

#define RSDP_SIGNATURE "RSD PTR "
#define RSDP_SIGNATURE_BYTES 8U
#define RSDP_V1_BYTES 20U
#define RSDP_FULL_BYTES 36U
#define RSDP_REVISION_2 2U

#define SIGNATURE_BYTES 4U
#define MADT_SIGNATURE "APIC"
#define FADT_SIGNATURE "FACP"
#define DSDT_SIGNATURE "DSDT"

#define FADT_DSDT 40U
#define FADT_SCI_INTERRUPT 46U
#define FADT_SMI_COMMAND 48U
#define FADT_ACPI_ENABLE 52U
#define FADT_ACPI_DISABLE 53U
#define FADT_PM1A_EVENT 56U
#define FADT_PM1B_EVENT 60U
#define FADT_PM1A_CONTROL 64U
#define FADT_PM1B_CONTROL 68U
#define FADT_PM1_EVENT_LENGTH 88U
#define FADT_PM1_CONTROL_LENGTH 89U
#define FADT_FLAGS 112U
#define FADT_RESET_REGISTER 116U
#define FADT_RESET_VALUE 128U
#define FADT_X_DSDT 140U

#define FADT_RESET_SUPPORTED 0x400U

#define GAS_SPACE 0U
#define GAS_ADDRESS 4U
#define GAS_SPACE_MEMORY 0U
#define GAS_SPACE_IO 1U

#define PM1_SCI_ENABLED 0x0001U
#define PM1_SLEEP_TYPE_SHIFT 10U
#define PM1_SLEEP_ENABLE 0x2000U

#define PM1_POWER_BUTTON 0x0100U

#define AML_ZERO 0x00U
#define AML_ONE 0x01U
#define AML_BYTE_PREFIX 0x0AU
#define AML_WORD_PREFIX 0x0BU
#define AML_DWORD_PREFIX 0x0CU
#define AML_PACKAGE 0x12U

#define MADT_LOCAL_APIC 0U
#define MADT_IO_APIC 1U
#define MADT_INTERRUPT_OVERRIDE 2U

#define MADT_CPU_ENABLED 1U
#define MADT_CPU_ONLINE_CAPABLE 2U

#define MADT_HEADER_BYTES 44U
#define MADT_LOCAL_APIC_ADDRESS_OFFSET 36U

#define OVERRIDE_POLARITY_MASK 0x3U
#define OVERRIDE_POLARITY_LOW 3U
#define OVERRIDE_TRIGGER_SHIFT 2U
#define OVERRIDE_TRIGGER_MASK 0x3U
#define OVERRIDE_TRIGGER_LEVEL 3U

struct acpi_header {
    char signature[SIGNATURE_BYTES];
    uint32_t length;
    uint8_t revision;
    uint8_t checksum;
    char oem_id[6];
    char oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} __attribute__((packed));

static struct acpi_machine machine;
static int parsed;
static struct acpi_power power;
static int power_known;

static int signature_is(const void *table, const char *expected, unsigned length) {
    const uint8_t *bytes = table;
    for (unsigned i = 0; i < length; i++)
        if (bytes[i] != (uint8_t)expected[i]) return 0;
    return 1;
}

static int checksum_ok(const void *table, uint32_t length) {
    const uint8_t *bytes = table;
    uint8_t total = 0;
    for (uint32_t i = 0; i < length; i++) total = (uint8_t)(total + bytes[i]);
    return total == 0;
}

#define ACPI_WINDOW_OFFSET 0x00100000ULL
#define ACPI_WINDOW_BYTES 0x00200000ULL
#define ACPI_PAGE_BYTES 4096ULL

static uint64_t window_used;

static const void *map_physical(uint64_t physical, uint32_t length) {
    if (!physical || !length) return NULL;
    uint64_t page = physical & ~(ACPI_PAGE_BYTES - 1);
    uint64_t offset = physical - page;
    uint64_t bytes = (offset + length + ACPI_PAGE_BYTES - 1) & ~(ACPI_PAGE_BYTES - 1);
    if (window_used + bytes > ACPI_WINDOW_BYTES) return NULL;

    uint64_t base = DEVICE_MMIO_VIRTUAL_BASE + ACPI_WINDOW_OFFSET + window_used;
    uint64_t cr3 = vmm_kernel_cr3();
    for (uint64_t i = 0; i < bytes; i += ACPI_PAGE_BYTES) {
        if (vmm_map_page_in(cr3, base + i, page + i,
                            PAGE_WRITE | PAGE_DEVICE | PAGE_NX) != 0) return NULL;
    }
    window_used += bytes;
    return (const void *)(base + offset);
}

static const struct acpi_header *map_table(uint64_t physical) {
    const struct acpi_header *header = map_physical(physical, sizeof(*header));
    if (!header) return NULL;
    return map_physical(physical, header->length);
}

static void parse_madt(const struct acpi_header *madt) {
    const uint8_t *base = (const uint8_t *)madt;
    machine.local_apic = *(const uint32_t *)(base + MADT_LOCAL_APIC_ADDRESS_OFFSET);

    for (uint32_t offset = MADT_HEADER_BYTES; offset + 2U <= madt->length; ) {
        uint8_t type = base[offset];
        uint8_t length = base[offset + 1U];
        if (!length || offset + length > madt->length) break;
        const uint8_t *entry = base + offset;

        if (type == MADT_IO_APIC && machine.io_apic_count < ACPI_MAX_IO_APICS) {
            struct acpi_io_apic *io = &machine.io_apics[machine.io_apic_count++];
            io->id = entry[2];
            io->address = *(const uint32_t *)(entry + 4);
            io->global_base = *(const uint32_t *)(entry + 8);
        } else if (type == MADT_INTERRUPT_OVERRIDE &&
                   machine.override_count < ACPI_MAX_OVERRIDES) {
            struct acpi_override *over = &machine.overrides[machine.override_count++];
            over->source = entry[3];
            over->global = *(const uint32_t *)(entry + 4);
            uint16_t flags = *(const uint16_t *)(entry + 8);
            over->active_low = (flags & OVERRIDE_POLARITY_MASK) == OVERRIDE_POLARITY_LOW;
            over->level_triggered =
                ((flags >> OVERRIDE_TRIGGER_SHIFT) & OVERRIDE_TRIGGER_MASK) ==
                OVERRIDE_TRIGGER_LEVEL;
        } else if (type == MADT_LOCAL_APIC) {
            machine.cpu_listed++;
            if (machine.cpu_count >= ACPI_MAX_CPUS) { offset += length; continue; }
            uint32_t flags = *(const uint32_t *)(entry + 4);
            struct acpi_cpu *cpu = &machine.cpus[machine.cpu_count++];
            cpu->acpi_id = entry[2];
            cpu->apic_id = entry[3];
            cpu->usable = (flags & (MADT_CPU_ENABLED | MADT_CPU_ONLINE_CAPABLE)) != 0;
        }
        offset += length;
    }
}

static const struct acpi_header *find_table(uint64_t root_physical, int wide,
                                            const char *signature) {
    const struct acpi_header *root = map_table(root_physical);
    if (!root || !checksum_ok(root, root->length)) return NULL;

    uint32_t entry_bytes = wide ? 8U : 4U;
    uint32_t count = (root->length - (uint32_t)sizeof(*root)) / entry_bytes;
    const uint8_t *entries = (const uint8_t *)root + sizeof(*root);

    for (uint32_t i = 0; i < count; i++) {
        uint64_t physical = wide ? *(const uint64_t *)(entries + i * entry_bytes)
                                 : *(const uint32_t *)(entries + i * entry_bytes);
        const struct acpi_header *table = map_table(physical);
        if (!table) continue;
        if (!signature_is(table->signature, signature, SIGNATURE_BYTES)) continue;
        if (!checksum_ok(table, table->length)) continue;
        return table;
    }
    return NULL;
}

static uint8_t table_u8(const uint8_t *table, uint32_t length, uint32_t offset) {
    return offset + 1U <= length ? table[offset] : 0;
}

static uint16_t table_u16(const uint8_t *table, uint32_t length, uint32_t offset) {
    return offset + 2U <= length ? *(const uint16_t *)(table + offset) : 0;
}

static uint32_t table_u32(const uint8_t *table, uint32_t length, uint32_t offset) {
    return offset + 4U <= length ? *(const uint32_t *)(table + offset) : 0;
}

static uint64_t table_u64(const uint8_t *table, uint32_t length, uint32_t offset) {
    return offset + 8U <= length ? *(const uint64_t *)(table + offset) : 0;
}

static int aml_integer(const uint8_t **cursor, const uint8_t *end, uint32_t *out) {
    const uint8_t *at = *cursor;
    if (at >= end) return -1;
    uint8_t opcode = *at++;
    switch (opcode) {
        case AML_ZERO: *out = 0; break;
        case AML_ONE:  *out = 1; break;
        case AML_BYTE_PREFIX:
            if (at + 1 > end) return -1;
            *out = at[0];
            at += 1;
            break;
        case AML_WORD_PREFIX:
            if (at + 2 > end) return -1;
            *out = (uint32_t)at[0] | ((uint32_t)at[1] << 8);
            at += 2;
            break;
        case AML_DWORD_PREFIX:
            if (at + 4 > end) return -1;
            *out = (uint32_t)at[0] | ((uint32_t)at[1] << 8) |
                   ((uint32_t)at[2] << 16) | ((uint32_t)at[3] << 24);
            at += 4;
            break;
        default: return -1;
    }
    *cursor = at;
    return 0;
}

static int parse_sleep_state(const struct acpi_header *dsdt) {
    const uint8_t *base = (const uint8_t *)dsdt;
    const uint8_t *end = base + dsdt->length;

    for (const uint8_t *at = base + sizeof(*dsdt); at + 4U <= end; at++) {
        if (at[0] != '_' || at[1] != 'S' || at[2] != '5' || at[3] != '_') continue;

        const uint8_t *cursor = at + 4;
        if (cursor >= end || *cursor != AML_PACKAGE) continue;
        cursor++;
        if (cursor >= end) continue;
        cursor += ((*cursor >> 6) & 0x3U) + 1U;
        if (cursor >= end) continue;

        uint8_t elements = *cursor++;
        if (elements < 1U) continue;

        uint32_t type_a = 0;
        uint32_t type_b = 0;
        if (aml_integer(&cursor, end, &type_a) != 0) continue;
        if (elements >= 2U && aml_integer(&cursor, end, &type_b) != 0) type_b = 0;

        power.sleep_type_a = (uint8_t)(type_a & 0x7U);
        power.sleep_type_b = (uint8_t)(type_b & 0x7U);
        power.sleep_known = 1;
        return 0;
    }
    return -1;
}

static void parse_fadt(const struct acpi_header *fadt) {
    const uint8_t *base = (const uint8_t *)fadt;
    uint32_t length = fadt->length;

    power.sci_interrupt = table_u16(base, length, FADT_SCI_INTERRUPT);
    power.smi_command = table_u32(base, length, FADT_SMI_COMMAND);
    power.enable_value = table_u8(base, length, FADT_ACPI_ENABLE);
    power.disable_value = table_u8(base, length, FADT_ACPI_DISABLE);
    power.pm1a_event = table_u32(base, length, FADT_PM1A_EVENT);
    power.pm1b_event = table_u32(base, length, FADT_PM1B_EVENT);
    power.pm1a_control = table_u32(base, length, FADT_PM1A_CONTROL);
    power.pm1b_control = table_u32(base, length, FADT_PM1B_CONTROL);
    power.event_bytes = table_u8(base, length, FADT_PM1_EVENT_LENGTH);
    power.control_bytes = table_u8(base, length, FADT_PM1_CONTROL_LENGTH);

    uint32_t flags = table_u32(base, length, FADT_FLAGS);
    if (flags & FADT_RESET_SUPPORTED) {
        uint8_t space = table_u8(base, length, FADT_RESET_REGISTER + GAS_SPACE);
        uint64_t address = table_u64(base, length, FADT_RESET_REGISTER + GAS_ADDRESS);
        if (address && (space == GAS_SPACE_IO || space == GAS_SPACE_MEMORY)) {
            power.reset_supported = 1;
            power.reset_space = space;
            power.reset_address = address;
            power.reset_value = table_u8(base, length, FADT_RESET_VALUE);
        }
    }

    uint64_t dsdt_physical = table_u64(base, length, FADT_X_DSDT);
    if (!dsdt_physical) dsdt_physical = table_u32(base, length, FADT_DSDT);
    if (dsdt_physical) {
        const struct acpi_header *dsdt = map_table(dsdt_physical);
        if (dsdt && signature_is(dsdt->signature, DSDT_SIGNATURE, SIGNATURE_BYTES))
            (void)parse_sleep_state(dsdt);
    }

    power_known = power.pm1a_control != 0;
}

static void parse_tables(void) {
    uint64_t rsdp_physical = boot_info()->rsdp;
    if (!rsdp_physical) return;
    const uint8_t *rsdp = map_physical(rsdp_physical, RSDP_FULL_BYTES);
    if (!rsdp || !signature_is(rsdp, RSDP_SIGNATURE, RSDP_SIGNATURE_BYTES)) return;
    if (!checksum_ok(rsdp, RSDP_V1_BYTES)) return;

    uint8_t revision = rsdp[15];
    uint64_t root = 0;
    int wide = 0;
    if (revision >= RSDP_REVISION_2) {
        root = *(const uint64_t *)(rsdp + 24);
        wide = 1;
    }
    if (!root) {
        root = *(const uint32_t *)(rsdp + 16);
        wide = 0;
    }
    if (!root) return;

    const struct acpi_header *madt = find_table(root, wide, MADT_SIGNATURE);
    if (!madt && wide) {
        uint64_t rsdt = *(const uint32_t *)(rsdp + 16);
        if (rsdt) {
            root = rsdt;
            wide = 0;
            madt = find_table(root, wide, MADT_SIGNATURE);
        }
    }
    if (madt) parse_madt(madt);

    const struct acpi_header *fadt = find_table(root, wide, FADT_SIGNATURE);
    if (fadt) parse_fadt(fadt);
}

const struct acpi_machine *acpi_describe_machine(void) {
    if (!parsed) {
        parsed = 1;
        parse_tables();

        if (machine.cpu_listed > machine.cpu_count)
            kprintf("ACPI: table lists %u cpus, only %u recorded\n",
                    (unsigned)machine.cpu_listed, (unsigned)machine.cpu_count);
        if (machine.local_apic)
            kprintf("ACPI: %u cpu(s), local apic at %x, %u ioapic(s), %u override(s)\n",
                    (unsigned)machine.cpu_count, (unsigned)machine.local_apic,
                    (unsigned)machine.io_apic_count, (unsigned)machine.override_count);
        if (power_known)
            kprintf("ACPI: pm1a at %x, sci %u, %s, reset %s\n",
                    (unsigned)power.pm1a_control, (unsigned)power.sci_interrupt,
                    power.sleep_known ? "s5 known" : "no s5 object",
                    power.reset_supported ? "register" : "keyboard controller");
        else
            kprintf("ACPI: no usable fadt; the machine cannot be powered off\n");
    }
    if (!machine.local_apic || !machine.io_apic_count) return NULL;
    return &machine;
}

const struct acpi_power *acpi_power_info(void) {
    (void)acpi_describe_machine();
    return power_known ? &power : NULL;
}

#define ACPI_HANDOVER_TIMEOUT_NS (3ULL * 1000ULL * 1000ULL * 1000ULL)
#define ACPI_SETTLE_NS (200ULL * 1000ULL * 1000ULL)

static void settle(uint64_t nanoseconds) {
    uint64_t deadline = time_uptime_ns() + nanoseconds;
    while (time_uptime_ns() < deadline) cpu_relax();
}

int acpi_enable(void) {
    if (!acpi_power_info()) return -1;
    if (inw((uint16_t)power.pm1a_control) & PM1_SCI_ENABLED) return 0;
    if (!power.smi_command || !power.enable_value) return -1;

    outb((uint16_t)power.smi_command, power.enable_value);
    uint64_t deadline = time_uptime_ns() + ACPI_HANDOVER_TIMEOUT_NS;
    while (!(inw((uint16_t)power.pm1a_control) & PM1_SCI_ENABLED)) {
        if (time_uptime_ns() > deadline) return -1;
        cpu_relax();
    }
    if (power.pm1b_control) {
        while (!(inw((uint16_t)power.pm1b_control) & PM1_SCI_ENABLED)) {
            if (time_uptime_ns() > deadline) return -1;
            cpu_relax();
        }
    }
    return 0;
}

void acpi_power_off(void) {
    if (!acpi_power_info() || !power.sleep_known) return;
    (void)acpi_enable();

    outw((uint16_t)power.pm1a_control,
         (uint16_t)((power.sleep_type_a << PM1_SLEEP_TYPE_SHIFT) | PM1_SLEEP_ENABLE));
    if (power.pm1b_control)
        outw((uint16_t)power.pm1b_control,
             (uint16_t)((power.sleep_type_b << PM1_SLEEP_TYPE_SHIFT) | PM1_SLEEP_ENABLE));

    settle(ACPI_SETTLE_NS);
}

void acpi_reset(void) {
    const struct acpi_power *info = acpi_power_info();
    if (info && info->reset_supported) {
        if (info->reset_space == GAS_SPACE_IO) {
            outb((uint16_t)info->reset_address, info->reset_value);
        } else {
            volatile uint8_t *reg =
                (volatile uint8_t *)map_physical(info->reset_address, 1);
            if (reg) *reg = info->reset_value;
        }
        settle(ACPI_SETTLE_NS);
    }

    for (unsigned spin = 0; spin < 100000U && (inb(0x64) & 0x02U); spin++) io_wait();
    outb(0x64, 0xFE);
    settle(ACPI_SETTLE_NS);

    struct { uint16_t limit; uint64_t base; } __attribute__((packed)) empty = { 0, 0 };
    __asm__ volatile("lidt %0; int3" : : "m"(empty));
    cpu_halt_forever();
}

static uint16_t event_enable_port(uint32_t event_block) {
    if (!event_block || power.event_bytes < 2U) return 0;
    return (uint16_t)(event_block + power.event_bytes / 2U);
}

void acpi_power_button_enable(unsigned vector) {
    if (!acpi_power_info() || !power.pm1a_event) return;
    if (acpi_enable() != 0) {
        kprintf("ACPI: firmware would not hand over the fixed hardware\n");
        return;
    }

    uint16_t enable = event_enable_port(power.pm1a_event);
    if (!enable) return;
    outw((uint16_t)power.pm1a_event, PM1_POWER_BUTTON);
    outw(enable, (uint16_t)(inw(enable) | PM1_POWER_BUTTON));
    if (power.pm1b_event) {
        uint16_t second = event_enable_port(power.pm1b_event);
        if (second) {
            outw((uint16_t)power.pm1b_event, PM1_POWER_BUTTON);
            outw(second, (uint16_t)(inw(second) | PM1_POWER_BUTTON));
        }
    }

    if (apic_route_global(power.sci_interrupt, vector) != 0) {
        kprintf("ACPI: sci %u is outside the ioapic; no power button\n",
                (unsigned)power.sci_interrupt);
        return;
    }
    kprintf("ACPI: power button on sci %u\n", (unsigned)power.sci_interrupt);
}

int acpi_sci_interrupt(void) {
    if (!power_known || !power.pm1a_event) return 0;

    int pressed = 0;
    if (inw((uint16_t)power.pm1a_event) & PM1_POWER_BUTTON) {
        outw((uint16_t)power.pm1a_event, PM1_POWER_BUTTON);
        pressed = 1;
    }
    if (power.pm1b_event && (inw((uint16_t)power.pm1b_event) & PM1_POWER_BUTTON)) {
        outw((uint16_t)power.pm1b_event, PM1_POWER_BUTTON);
        pressed = 1;
    }
    return pressed;
}
