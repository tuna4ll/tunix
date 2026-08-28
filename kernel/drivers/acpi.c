/*
 * Just enough ACPI to find the interrupt controllers and to stop the machine.
 *
 * The 8259 PIC this kernel started on is a compatibility device. On real
 * hardware the interrupt a PCI card raises does not arrive on the line its
 * config space names -- the routing goes through an IOAPIC, and which of its
 * inputs a device lands on is something only the firmware's tables say. So
 * before anything can be routed, the MADT has to be read.
 *
 * Turning the machine off is the other thing only ACPI can do. There is no
 * port to write to that means "power down": the FADT names the ports, and the
 * *value* to write lives in the DSDT as an AML object called \_S5_. So this
 * does interpret a few bytes of AML after all -- but only enough to read one
 * package of small integers out of it, which is a long way from an interpreter
 * and is all that a machine which never sleeps or changes power state needs.
 */
#include <stdint.h>
#include <stddef.h>

#include "../include/acpi.h"
#include "../include/apic.h"
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
/* "FACP" is the FADT. The signature and the name of the table have differed
   since ACPI 1.0 and the specification has never fixed it. */
#define FADT_SIGNATURE "FACP"
#define DSDT_SIGNATURE "DSDT"

/* FADT field offsets. Named rather than expressed as a struct because the
   table has grown four times and a struct would either claim fields a short
   table does not have or stop at the ones ACPI 1.0 knew about. Every read
   below is guarded by the table's own length. */
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

/* FADT flags: the only one that matters here says the reset register is real
   rather than a field of zeroes. */
#define FADT_RESET_SUPPORTED 0x400U

/* Generic address structure, as the FADT embeds it. */
#define GAS_SPACE 0U
#define GAS_ADDRESS 4U
#define GAS_SPACE_MEMORY 0U
#define GAS_SPACE_IO 1U

/* PM1 control register. SCI_EN is how the machine says ACPI mode is on; the
   sleep type goes in the middle and SLP_EN is the write that acts on it. */
#define PM1_SCI_ENABLED 0x0001U
#define PM1_SLEEP_TYPE_SHIFT 10U
#define PM1_SLEEP_ENABLE 0x2000U

/* PM1 event register, whose enable half sits directly above its status half.
   Only the power button is listened for. */
#define PM1_POWER_BUTTON 0x0100U

/* AML opcodes, the handful a package of small integers can be made of. */
#define AML_ZERO 0x00U
#define AML_ONE 0x01U
#define AML_BYTE_PREFIX 0x0AU
#define AML_WORD_PREFIX 0x0BU
#define AML_DWORD_PREFIX 0x0CU
#define AML_PACKAGE 0x12U

/* MADT entry types. Only the three that decide routing are read. */
#define MADT_LOCAL_APIC 0U
#define MADT_IO_APIC 1U
#define MADT_INTERRUPT_OVERRIDE 2U

/* A processor entry describes a socket that may be empty. Enabled means it is
   there and running; online-capable means the firmware could bring it up
   later. Anything with neither bit is a hole in the table, and sending it an
   INIT would wait for a processor that never answers. */
#define MADT_CPU_ENABLED 1U
#define MADT_CPU_ONLINE_CAPABLE 2U

#define MADT_HEADER_BYTES 44U
#define MADT_LOCAL_APIC_ADDRESS_OFFSET 36U

/* Polarity and trigger live in the same 16-bit flags word of an override. */
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

/* The kernel's string helpers have no memcmp, and a table signature is the
   only thing here that needs one. */
static int signature_is(const void *table, const char *expected, unsigned length) {
    const uint8_t *bytes = table;
    for (unsigned i = 0; i < length; i++)
        if (bytes[i] != (uint8_t)expected[i]) return 0;
    return 1;
}

/* Every ACPI table is checksummed by its bytes summing to zero in eight bits,
   which is the only way to tell a table from whatever else is at that
   address on a machine whose firmware got the pointer wrong. */
static int checksum_ok(const void *table, uint32_t length) {
    const uint8_t *bytes = table;
    uint8_t total = 0;
    for (uint32_t i = 0; i < length; i++) total = (uint8_t)(total + bytes[i]);
    return total == 0;
}

/*
 * ACPI tables are not in the direct map and cannot be.
 *
 * Firmware puts them just under the top of low memory -- on a 2 GiB machine
 * that is around 0x7FFE0000, while the direct map stops at PMM_DIRECT_MAP_LIMIT
 * (1792 MiB). Handing such an address to vmm_phys_to_virt() panics, which is
 * the right answer to the question it was asked and the wrong one here. So
 * these get a window of their own, next to the one the xHCI registers use.
 */
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

/* A table states its own length, so it takes two goes: the header first, then
   the whole of it. The window is large enough to spend a page finding out. */
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

/* Walk the RSDT or XSDT looking for one table. Both are arrays of pointers to
   tables; the only difference is how wide the pointers are. */
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

/* --- the fixed hardware ------------------------------------------------- */

/*
 * The FADT has grown four times and every version kept the old fields where
 * they were, so a short table is not a broken one -- it is an old one, and the
 * fields past its end simply do not exist. Reading through accessors that
 * answer zero beyond the length is what makes that safe to ignore everywhere
 * else.
 */
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

/* One AML integer, which for a sleep package is always one of five encodings:
   the two constants, or a prefix byte followed by one, two or four bytes. */
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

/*
 * Find \_S5_ in the DSDT and read the sleep type out of it.
 *
 * This is the one place ACPI cannot be treated as a table of numbers: the
 * ports to write are in the FADT, but the value that means "off" is an AML
 * object, and different firmware picks different values for it. Interpreting
 * AML properly means a bytecode machine with a namespace and an operation
 * region driver, which is a subsystem, not a function.
 *
 * What is done instead is a scan for the four-character name followed by a
 * package of small integers. The name in AML is always four characters, short
 * ones padded with underscores, so what the source calls _S5 is "_S5_" here.
 * A false match -- those bytes appearing inside something else -- is rejected
 * by the package opcode that has to follow, and if one somehow got through the
 * result would be a machine that declined to power off rather than one that
 * did something else.
 */
static int parse_sleep_state(const struct acpi_header *dsdt) {
    const uint8_t *base = (const uint8_t *)dsdt;
    const uint8_t *end = base + dsdt->length;

    for (const uint8_t *at = base + sizeof(*dsdt); at + 4U <= end; at++) {
        if (at[0] != '_' || at[1] != 'S' || at[2] != '5' || at[3] != '_') continue;

        const uint8_t *cursor = at + 4;
        if (cursor >= end || *cursor != AML_PACKAGE) continue;
        cursor++;
        /* PkgLength, whose first byte's top two bits say how many more carry
           the rest of the count. The length itself is not needed; the package
           ends where the table does as far as this is concerned. */
        if (cursor >= end) continue;
        cursor += ((*cursor >> 6) & 0x3U) + 1U;
        if (cursor >= end) continue;

        uint8_t elements = *cursor++;
        if (elements < 1U) continue;

        uint32_t type_a = 0;
        uint32_t type_b = 0;
        if (aml_integer(&cursor, end, &type_a) != 0) continue;
        /* A package with only one element leaves the second block at zero,
           which is right: a machine with no PM1b has nothing to write. */
        if (elements >= 2U && aml_integer(&cursor, end, &type_b) != 0) type_b = 0;

        /* Three bits is all the field has; anything wider is a misread. */
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

    /* The wide pointer wins where the table is long enough to have one: a
       machine with its DSDT above 4 GiB leaves the narrow field zero. */
    uint64_t dsdt_physical = table_u64(base, length, FADT_X_DSDT);
    if (!dsdt_physical) dsdt_physical = table_u32(base, length, FADT_DSDT);
    if (dsdt_physical) {
        const struct acpi_header *dsdt = map_table(dsdt_physical);
        /* Not checksummed. Plenty of shipping firmware gets the DSDT's
           checksum wrong, and refusing to read it would cost those machines
           the ability to power off over a byte nothing else depends on. */
        if (dsdt && signature_is(dsdt->signature, DSDT_SIGNATURE, SIGNATURE_BYTES))
            (void)parse_sleep_state(dsdt);
    }

    /* Without the control block there is nowhere to write, and everything
       below has to decline rather than write to port zero. */
    power_known = power.pm1a_control != 0;
}

/* --- reading the tables ------------------------------------------------- */

static void parse_tables(void) {
    uint64_t rsdp_physical = boot_info()->rsdp;
    if (!rsdp_physical) return;
    const uint8_t *rsdp = map_physical(rsdp_physical, RSDP_FULL_BYTES);
    if (!rsdp || !signature_is(rsdp, RSDP_SIGNATURE, RSDP_SIGNATURE_BYTES)) return;
    if (!checksum_ok(rsdp, RSDP_V1_BYTES)) return;

    /* Revision 2 machines have both a 32-bit RSDT and a 64-bit XSDT, and the
       specification says to prefer the wide one where it exists. */
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
        /* A revision 2 machine whose XSDT is unusable still has the older
           table, and the two describe the same machine. */
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
    /* An IOAPIC is what the caller is really asking about: without one there is
       nothing to route through and it has to stay on the 8259 pair. */
    if (!machine.local_apic || !machine.io_apic_count) return NULL;
    return &machine;
}

const struct acpi_power *acpi_power_info(void) {
    (void)acpi_describe_machine();
    return power_known ? &power : NULL;
}

/* --- stopping the machine ----------------------------------------------- */

#define ACPI_HANDOVER_TIMEOUT_NS (3ULL * 1000ULL * 1000ULL * 1000ULL)
/* Long enough for a write to a power register to take effect, short enough
   that a machine which ignored it still gets the next thing tried. */
#define ACPI_SETTLE_NS (200ULL * 1000ULL * 1000ULL)

static void settle(uint64_t nanoseconds) {
    uint64_t deadline = time_uptime_ns() + nanoseconds;
    while (time_uptime_ns() < deadline) __asm__ volatile("pause");
}

int acpi_enable(void) {
    if (!acpi_power_info()) return -1;
    /* Already ours: firmware that boots through UEFI usually hands the machine
       over with ACPI mode on, and asking again is not harmless. */
    if (inw((uint16_t)power.pm1a_control) & PM1_SCI_ENABLED) return 0;
    if (!power.smi_command || !power.enable_value) return -1;

    outb((uint16_t)power.smi_command, power.enable_value);
    uint64_t deadline = time_uptime_ns() + ACPI_HANDOVER_TIMEOUT_NS;
    while (!(inw((uint16_t)power.pm1a_control) & PM1_SCI_ENABLED)) {
        if (time_uptime_ns() > deadline) return -1;
        __asm__ volatile("pause");
    }
    if (power.pm1b_control) {
        while (!(inw((uint16_t)power.pm1b_control) & PM1_SCI_ENABLED)) {
            if (time_uptime_ns() > deadline) return -1;
            __asm__ volatile("pause");
        }
    }
    return 0;
}

void acpi_power_off(void) {
    if (!acpi_power_info() || !power.sleep_known) return;
    /* The sleep registers only answer once the firmware has handed them over,
       so a machine still in legacy mode has to be asked first. */
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

    /*
     * The keyboard controller's reset line, which predates ACPI by a decade
     * and is still wired on every PC. Its input buffer has to be empty before
     * the command will be taken.
     */
    for (unsigned spin = 0; spin < 100000U && (inb(0x64) & 0x02U); spin++) io_wait();
    outb(0x64, 0xFE);
    settle(ACPI_SETTLE_NS);

    /*
     * Nothing left to ask politely. An empty interrupt table means the
     * processor cannot deliver the breakpoint, cannot deliver the double fault
     * that follows, and resets -- which is the outcome wanted, reached the
     * only way still available.
     */
    struct { uint16_t limit; uint64_t base; } __attribute__((packed)) empty = { 0, 0 };
    __asm__ volatile("lidt %0; int3" : : "m"(empty));
    for (;;) __asm__ volatile("cli; hlt");
}

/* The enable half of an event block sits directly above its status half, and
   the block's length covers both. */
static uint16_t event_enable_port(uint32_t event_block) {
    if (!event_block || power.event_bytes < 2U) return 0;
    return (uint16_t)(event_block + power.event_bytes / 2U);
}

/*
 * Unmask the fixed-feature power button.
 *
 * Untested against a press, and not for want of trying. QEMU's `pc` machine
 * says in its own FADT that the button is a fixed feature (flags bit 4 clear),
 * and everything this sets up reads back correct from the guest: ACPI mode on,
 * PWRBTN_EN set in the enable register, the PM block answering -- its timer
 * counts, so it is the real device and not an unclaimed port range. QEMU emits
 * its POWERDOWN event when asked, and PWRBTN_STS never appears. Nothing this
 * side can do about that, so what is here is what the tables ask for; a machine
 * that raises the event will be answered.
 */
void acpi_power_button_enable(unsigned vector) {
    if (!acpi_power_info() || !power.pm1a_event) return;
    if (acpi_enable() != 0) {
        kprintf("ACPI: firmware would not hand over the fixed hardware\n");
        return;
    }

    uint16_t enable = event_enable_port(power.pm1a_event);
    if (!enable) return;
    /* Status bits are cleared by writing one to them. Doing it before the
       enable stops a press that happened while nobody was listening from
       arriving as an interrupt the moment one is. */
    outw((uint16_t)power.pm1a_event, PM1_POWER_BUTTON);
    outw(enable, (uint16_t)(inw(enable) | PM1_POWER_BUTTON));
    if (power.pm1b_event) {
        uint16_t second = event_enable_port(power.pm1b_event);
        if (second) {
            outw((uint16_t)power.pm1b_event, PM1_POWER_BUTTON);
            outw(second, (uint16_t)(inw(second) | PM1_POWER_BUTTON));
        }
    }

    /* Through the MADT rather than on the ACPI default: the specification says
       the SCI is level-triggered and active low, and the override says what it
       is on this machine, which is not always the same thing. */
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
