#include <stddef.h>
#include <stdint.h>

#include <tunix/acpi.h>
#include <tunix/errno.h>
#include <tunix/module.h>
#include <tunix/power.h>
#include <uacpi/event.h>
#include <uacpi/notify.h>
#include <uacpi/uacpi.h>
#include <uacpi/utilities.h>

#include "priv.h"
#include <tunix/kprintf.h>

#define NOTIFY_STATUS_CHANGE 0x80U
#define BUTTONS_MAX          8U

static struct {
    uacpi_namespace_node *node;
    uacpi_notify_handler handler;
} buttons[BUTTONS_MAX];
static unsigned button_count;
static int fixed_button;

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
    if (button_count == BUTTONS_MAX) return UACPI_ITERATION_DECISION_BREAK;
    if (uacpi_install_notify_handler(node, handler, NULL) != UACPI_STATUS_OK) {
        char path[64];
        kprintf("ACPI: no notify handler on %s\n", acpi_node_path(node, path, sizeof(path)));
        return UACPI_ITERATION_DECISION_CONTINUE;
    }
    buttons[button_count].node = node;
    buttons[button_count].handler = handler;
    button_count++;
    return UACPI_ITERATION_DECISION_CONTINUE;
}

static int button_start(void) {
    if (!acpi_subsystem_ready()) return -ENODEV;
    uacpi_status status =
        uacpi_install_fixed_event_handler(UACPI_FIXED_EVENT_POWER_BUTTON, fixed_power_button, NULL);
    fixed_button = status == UACPI_STATUS_OK;
    if (fixed_button) kprintf("ACPI: power button\n");
    (void)uacpi_find_devices("PNP0C0C", attach, (void *)power_button_notify);
    (void)uacpi_find_devices("PNP0C0D", attach, (void *)lid_notify);
    return 0;
}

static void button_stop(void) {
    if (fixed_button) (void)uacpi_uninstall_fixed_event_handler(UACPI_FIXED_EVENT_POWER_BUTTON);
    fixed_button = 0;
    for (unsigned index = 0; index < button_count; index++)
        (void)uacpi_uninstall_notify_handler(buttons[index].node, buttons[index].handler);
    button_count = 0;
}

MODULE_MAIN(button_start, button_stop);
MODULE_ALIAS("acpi*:PNP0C0C:*");
MODULE_ALIAS("acpi*:PNP0C0D:*");
MODULE_ALIAS("acpi*:LNXPWRBN:*");
MODULE_LICENSE("MIT");
MODULE_DESCRIPTION("ACPI power button and lid");
MODULE_AUTHOR("Tunix");
