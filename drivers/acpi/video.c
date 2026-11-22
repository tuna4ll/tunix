#include <stddef.h>
#include <stdint.h>

#include <tunix/acpi.h>
#include <tunix/backlight.h>
#include <tunix/errno.h>
#include <tunix/module.h>
#include <tunix/input.h>
#include <tunix/time.h>
#include <uacpi/namespace.h>
#include <uacpi/notify.h>
#include <uacpi/types.h>
#include <uacpi/uacpi.h>

#include "priv.h"

#include <uapi/input_event.h>
#include <tunix/kprintf.h>

#define NOTIFY_BRIGHTNESS_UP   0x86U
#define NOTIFY_BRIGHTNESS_DOWN 0x87U
#define DOS_FIRMWARE_LEAVES    4U
#define REPEAT_WINDOW_NS       20000000ULL

#define OUTPUTS_MAX 8U

static uacpi_namespace_node *outputs[OUTPUTS_MAX];
static unsigned output_count;
static uint16_t last_key;
static uint64_t last_key_ns;

static uacpi_status output_notify(uacpi_handle context, uacpi_namespace_node *node,
                                  uacpi_u64 value) {
    (void)context;
    (void)node;
    uint16_t key = value == NOTIFY_BRIGHTNESS_UP ? TUNIX_KEY_BRIGHTNESSUP
        : value == NOTIFY_BRIGHTNESS_DOWN        ? TUNIX_KEY_BRIGHTNESSDOWN
                                                 : 0;
    if (!key) return UACPI_STATUS_OK;
    uint64_t now = time_uptime_ns();
    if (key == last_key && now - last_key_ns < REPEAT_WINDOW_NS) return UACPI_STATUS_OK;
    last_key = key;
    last_key_ns = now;
    if (!input_report_hotkey(key)) (void)backlight_step(key == TUNIX_KEY_BRIGHTNESSUP);
    return UACPI_STATUS_OK;
}

static void claim_brightness(uacpi_namespace_node *bus) {
    uacpi_namespace_node *method = NULL;
    if (uacpi_namespace_node_find(bus, "_DOS", &method) != UACPI_STATUS_OK) return;
    uacpi_object *argument = uacpi_object_create_integer(DOS_FIRMWARE_LEAVES);
    if (!argument) return;
    uacpi_object_array arguments = {&argument, 1};
    (void)uacpi_execute(bus, "_DOS", &arguments);
    uacpi_object_unref(argument);
}

static uacpi_iteration_decision find_output(void *user, uacpi_namespace_node *node,
                                            uacpi_u32 depth) {
    (void)user;
    (void)depth;
    uacpi_namespace_node *method = NULL;
    if (output_count == OUTPUTS_MAX) return UACPI_ITERATION_DECISION_BREAK;
    if (uacpi_namespace_node_find(node, "_BCM", &method) != UACPI_STATUS_OK)
        return UACPI_ITERATION_DECISION_CONTINUE;
    if (!acpi_device_present(node)) return UACPI_ITERATION_DECISION_CONTINUE;
    char path[64];
    if (uacpi_install_notify_handler(node, output_notify, NULL) != UACPI_STATUS_OK) {
        kprintf("ACPI: no notify handler on %s\n", acpi_node_path(node, path, sizeof(path)));
        return UACPI_ITERATION_DECISION_CONTINUE;
    }
    uacpi_namespace_node *bus = uacpi_namespace_node_parent(node);
    if (bus) claim_brightness(bus);
    outputs[output_count++] = node;
    __atomic_add_fetch(&acpi_video_output_count, 1U, __ATOMIC_RELAXED);
    kprintf("ACPI: video output %s\n", acpi_node_path(node, path, sizeof(path)));
    return UACPI_ITERATION_DECISION_CONTINUE;
}

static int video_start(void) {
    if (!acpi_subsystem_ready()) return -ENODEV;
    (void)uacpi_namespace_for_each_child(uacpi_namespace_root(), find_output, NULL,
                                         UACPI_OBJECT_DEVICE_BIT, UACPI_MAX_DEPTH_ANY, NULL);
    return 0;
}

static void video_stop(void) {
    for (unsigned index = 0; index < output_count; index++)
        (void)uacpi_uninstall_notify_handler(outputs[index], output_notify);
    __atomic_sub_fetch(&acpi_video_output_count, output_count, __ATOMIC_RELAXED);
    output_count = 0;
}

MODULE_MAIN(video_start, video_stop);
MODULE_ALIAS("acpi*:LNXVIDEO:*");
MODULE_LICENSE("MIT");
MODULE_DESCRIPTION("ACPI display brightness keys");
MODULE_AUTHOR("Tunix");
