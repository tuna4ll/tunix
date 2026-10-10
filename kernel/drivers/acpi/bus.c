#include <stddef.h>
#include <stdint.h>

#include <tunix/acpi.h>
#include <tunix/boot.h>
#include <tunix/kstring.h>
#include <tunix/time.h>
#include <uacpi/acpi.h>
#include <uacpi/event.h>
#include <uacpi/namespace.h>
#include <uacpi/sleep.h>
#include <uacpi/uacpi.h>
#include <uacpi/utilities.h>

#include "priv.h"

extern void kprintf(const char *fmt, ...);

static int subsystem_ready;

int acpi_device_present(uacpi_namespace_node *node) {
    uacpi_u32 flags = 0;
    if (uacpi_eval_sta(node, &flags) != UACPI_STATUS_OK) return 0;
    return (flags & ACPI_STA_RESULT_DEVICE_PRESENT) != 0;
}

const char *acpi_node_path(uacpi_namespace_node *node, char *buffer, unsigned size) {
    const uacpi_char *path = uacpi_namespace_node_generate_absolute_path(node);
    if (!size) return buffer;
    buffer[0] = '\0';
    if (!path) return buffer;
    unsigned used = 0;
    while (path[used] && used + 1U < size) {
        buffer[used] = path[used];
        used++;
    }
    buffer[used] = '\0';
    uacpi_free_absolute_path(path);
    return buffer;
}

static int step(const char *what, uacpi_status status) {
    if (status == UACPI_STATUS_OK) return 0;
    kprintf("ACPI: %s failed: %s\n", what, uacpi_status_to_string(status));
    return -1;
}

void acpi_subsystem_init(void) {
    if (!boot_info()->rsdp) return;
    const char *choice = boot_command_line_value("acpi");
    if (choice && strncmp(choice, "off", 3) == 0) {
        kprintf("ACPI: disabled on the command line\n");
        return;
    }
    if (acpi_host_start() != 0) {
        kprintf("ACPI: cannot start the event threads\n");
        return;
    }
    uint64_t started = time_uptime_ns();
    if (step("initialization", uacpi_initialize(0)) != 0) return;
    if (step("namespace load", uacpi_namespace_load()) != 0) return;
    (void)step("interrupt model", uacpi_set_interrupt_model(UACPI_INTERRUPT_MODEL_IOAPIC));
    int embedded = acpi_ec_probe() == 0;
    if (step("namespace initialization", uacpi_namespace_initialize()) != 0) return;
    subsystem_ready = 1;
    acpi_processor_probe();
    acpi_button_probe();
    acpi_video_probe();
    acpi_thermal_probe();
    if (embedded) acpi_ec_start();
    (void)step("gpe setup", uacpi_finalize_gpe_initialization());
    kprintf("ACPI: ready in %u ms\n", (unsigned)((time_uptime_ns() - started) / 1000000ULL));
}

int acpi_subsystem_ready(void) { return subsystem_ready; }

void acpi_describe_subsystem(struct acpi_subsystem_info *out) {
    out->ready = subsystem_ready;
    out->embedded_controller = acpi_ec_present();
    out->ec_events = acpi_ec_events();
    out->thermal_zones = acpi_thermal_zones();
    out->video_outputs = acpi_video_outputs();
}

int acpi_subsystem_power_off(void) {
    if (!subsystem_ready) return -1;
    if (uacpi_prepare_for_sleep_state(UACPI_SLEEP_STATE_S5) != UACPI_STATUS_OK) return -1;
    return uacpi_enter_sleep_state(UACPI_SLEEP_STATE_S5) == UACPI_STATUS_OK ? 0 : -1;
}

int acpi_subsystem_reboot(void) {
    if (!subsystem_ready) return -1;
    return uacpi_reboot() == UACPI_STATUS_OK ? 0 : -1;
}
