#ifndef TUNIX_ACPI_H
#define TUNIX_ACPI_H

#include <stdint.h>

#define ACPI_MAX_IO_APICS  128U
#define ACPI_MAX_OVERRIDES 256U

#define ACPI_MAX_CPUS 256U

struct acpi_io_apic {
    uint8_t id;
    uint32_t address;

    uint32_t global_base;
};

struct acpi_override {
    uint8_t source;
    uint32_t global;
    uint8_t active_low;
    uint8_t level_triggered;
};

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

const struct acpi_machine *acpi_describe_machine(void);

struct acpi_power {
    uint32_t smi_command;
    uint8_t enable_value;
    uint8_t disable_value;
    uint32_t pm1a_control;
    uint32_t pm1b_control;
    uint32_t pm1a_event;
    uint32_t pm1b_event;
    uint8_t control_bytes;
    uint8_t event_bytes;
    uint32_t sci_interrupt;
    uint8_t reset_supported;
    uint8_t reset_space;
    uint64_t reset_address;
    uint8_t reset_value;

    uint8_t sleep_type_a;
    uint8_t sleep_type_b;
    uint8_t sleep_known;

    uint32_t gpe0_block;
    uint32_t gpe1_block;
    uint8_t gpe0_length;
    uint8_t gpe1_length;
    uint8_t embedded_controller;
    uint32_t thermal_zones;
};

struct acpi_events {
    int sci_enabled_at_boot;
    int handed_over;
    const char *decision;
    uint64_t sci_count;
    uint64_t gpe_events;
    uint64_t button_events;
    int gpe_seen;
    uint32_t gpe_enabled_at_boot;
};

const struct acpi_events *acpi_event_state(void);

const struct acpi_power *acpi_power_info(void);

const void *acpi_table_at(unsigned index, char signature[4], uint32_t *length);

int acpi_enable(void);

void acpi_power_off(void);

void acpi_reset(void) __attribute__((noreturn));

#define ACPI_SCI_VECTOR 0x30U

void acpi_power_button_enable(unsigned vector);

int acpi_sci_interrupt(void);

#endif
