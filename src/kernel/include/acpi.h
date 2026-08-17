#ifndef TUNIX_ACPI_H
#define TUNIX_ACPI_H

#include <stdint.h>

/* One machine has one IOAPIC in every configuration this kernel will meet; the
   room for more is because the table is allowed to list them and dropping the
   extras silently would be a lie. */
#define ACPI_MAX_IO_APICS 4U
#define ACPI_MAX_OVERRIDES 16U
/* The MADT may list more; the extras are counted and dropped, and the count is
   what /proc/cpuinfo reports, so an over-long table is visible rather than
   silently rounded down. */
#define ACPI_MAX_CPUS 32U

struct acpi_io_apic {
    uint8_t id;
    uint32_t address;
    /* The first global interrupt number this controller's inputs answer to. */
    uint32_t global_base;
};

/* The firmware saying "the legacy line you know as `source` is really wired to
   global interrupt `global`, and it is not active-high edge-triggered like the
   PIC's were". IRQ 0 arriving on global 2 is the usual one. */
struct acpi_override {
    uint8_t source;
    uint32_t global;
    uint8_t active_low;
    uint8_t level_triggered;
};

/* One processor the firmware says exists. `apic_id` is what an INIT/SIPI pair
   is addressed to, and it is not the index: firmware is free to number the
   processors however it likes, and on a machine with hyperthreading disabled
   in the BIOS the ids that remain are not contiguous. */
struct acpi_cpu {
    uint8_t acpi_id;
    uint8_t apic_id;
    uint8_t usable;
};

struct acpi_machine {
    uint32_t local_apic;
    uint32_t cpu_count;
    uint32_t cpu_listed;
    struct acpi_cpu cpus[ACPI_MAX_CPUS];
    uint32_t io_apic_count;
    struct acpi_io_apic io_apics[ACPI_MAX_IO_APICS];
    uint32_t override_count;
    struct acpi_override overrides[ACPI_MAX_OVERRIDES];
};

/* Parsed once and remembered. NULL when the machine has no usable tables, in
   which case the caller should stay on the 8259 pair. */
const struct acpi_machine *acpi_describe_machine(void);

/*
 * The fixed hardware the FADT describes: the ports that turn ACPI on, put the
 * machine to sleep and reset it.
 *
 * Separate from acpi_machine because it comes from a different table and is
 * wanted at a different time -- the MADT is read before interrupts are routed,
 * this is read once and used when somebody asks the machine to stop.
 */
struct acpi_power {
    uint32_t smi_command;      /* where to ask the firmware for ACPI mode */
    uint8_t enable_value;
    uint8_t disable_value;
    uint32_t pm1a_control;     /* SLP_TYP and SLP_EN are written here */
    uint32_t pm1b_control;     /* zero on a machine with only one block */
    uint32_t pm1a_event;       /* status, with the enable register above it */
    uint32_t pm1b_event;
    uint8_t control_bytes;
    uint8_t event_bytes;
    uint32_t sci_interrupt;    /* a global interrupt number, not a legacy IRQ */
    uint8_t reset_supported;
    uint8_t reset_space;       /* generic-address space id: 0 memory, 1 port */
    uint64_t reset_address;
    uint8_t reset_value;
    /* The value the \_S5_ object in the DSDT says means "off". Without it the
       machine can be reset but not powered down, so the two are reported
       apart rather than together. */
    uint8_t sleep_type_a;
    uint8_t sleep_type_b;
    uint8_t sleep_known;
};

/* NULL on a machine with no FADT, which is every machine this will not run on
   and one or two it might. */
const struct acpi_power *acpi_power_info(void);

/* Ask the firmware to hand the fixed hardware over. Zero once ACPI mode is on,
   including when it already was; -1 when the machine cannot be asked. */
int acpi_enable(void);

/* Enter S5. Returns only if the machine refused, which means the caller has to
   have something else to try. */
void acpi_power_off(void);

/* Reset through the FADT's register, then the keyboard controller, then a
   triple fault. Does not return. */
void acpi_reset(void) __attribute__((noreturn));

/* Where the SCI is delivered. Above the legacy lines the PIC was remapped onto
   and well below the ones the local APIC uses for itself. */
#define ACPI_SCI_VECTOR 0x30U

/* Unmask the power button and route its interrupt to `vector`. Quietly does
   nothing on a machine that describes no event block. */
void acpi_power_button_enable(unsigned vector);
/* Acknowledge one SCI. True when the power button is what raised it. */
int acpi_sci_interrupt(void);

#endif
