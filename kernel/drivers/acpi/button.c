#include <stddef.h>
#include <stdint.h>

#include <tunix/power.h>
#include <uacpi/event.h>
#include <uacpi/notify.h>
#include <uacpi/uacpi.h>
#include <uacpi/utilities.h>

#include "priv.h"

extern void kprintf(const char *fmt, ...);

#define NOTIFY_STATUS_CHANGE 0x80U

static uacpi_interrupt_ret fixed_power_button(uacpi_handle context) {
    (void)context;
    power_button_pressed();
    return UACPI_INTERRUPT_HANDLED;
}

static uacpi_status power_button_notify(uacpi_handle context, uacpi_namespace_node *node,
                                        uacpi_u64 value) {
    (void)context;
    (void)node;
    if (value == NOTIFY_STATUS_CHANGE) power_button_pressed();
    return UACPI_STATUS_OK;
}

static uacpi_status lid_notify(uacpi_handle context, uacpi_namespace_node *node, uacpi_u64 value) {
    (void)context;
    if (value != NOTIFY_STATUS_CHANGE) return UACPI_STATUS_OK;
    uint64_t open = 1;
    if (uacpi_eval_simple_integer(node, "_LID", &open) == UACPI_STATUS_OK)
        kprintf("ACPI: lid %s\n", open ? "open" : "closed");
    return UACPI_STATUS_OK;
}

static uacpi_iteration_decision attach(void *user, uacpi_namespace_node *node, uacpi_u32 depth) {
    (void)depth;
    uacpi_notify_handler handler = (uacpi_notify_handler)user;
    if (uacpi_install_notify_handler(node, handler, NULL) != UACPI_STATUS_OK) {
        char path[64];
        kprintf("ACPI: no notify handler on %s\n", acpi_node_path(node, path, sizeof(path)));
    }
    return UACPI_ITERATION_DECISION_CONTINUE;
}

void acpi_button_probe(void) {
    uacpi_status status =
        uacpi_install_fixed_event_handler(UACPI_FIXED_EVENT_POWER_BUTTON, fixed_power_button, NULL);
    if (status == UACPI_STATUS_OK) kprintf("ACPI: power button\n");
    (void)uacpi_find_devices("PNP0C0C", attach, (void *)power_button_notify);
    (void)uacpi_find_devices("PNP0C0D", attach, (void *)lid_notify);
}
