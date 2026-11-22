#include <stddef.h>
#include <stdint.h>

#include <tunix/acpi.h>
#include <tunix/boot.h>
#include <tunix/kstring.h>
#include <tunix/sysfs.h>
#include <tunix/time.h>
#include <uacpi/acpi.h>
#include <uacpi/tables.h>
#include <uacpi/event.h>
#include <uacpi/namespace.h>
#include <uacpi/notify.h>
#include <uacpi/sleep.h>
#include <uacpi/uacpi.h>
#include <uacpi/utilities.h>

#include "priv.h"
#include <tunix/kprintf.h>

static int subsystem_ready;
static uint64_t notifications;

// Kept here rather than in the drivers, which are modules and may not be loaded.
unsigned acpi_thermal_zone_count;
unsigned acpi_video_output_count;

static uacpi_status system_notify(uacpi_handle context, uacpi_namespace_node *node,
                                  uacpi_u64 value) {
    (void)context;
    (void)node;
    (void)value;
    __atomic_add_fetch(&notifications, 1U, __ATOMIC_RELAXED);
    return UACPI_STATUS_OK;
}

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

#define PUBLISHED_IDS_MAX 64U

static struct {
    char id[16];
    unsigned count;
} published_ids[PUBLISHED_IDS_MAX];

static void append(char *out, unsigned size, unsigned *used, const char *text) {
    while (*text && *used + 1U < size) out[(*used)++] = *text++;
    out[*used] = '\0';
}

// Publishes one device as ID:NN, numbered per ID as Linux does, with a modalias of
// its ID followed by its compatible IDs: "acpi:PNP0C0A:PNP0C0B:".
static void publish(uacpi_namespace_node *node, const char *id, const uacpi_pnp_id_list *cids) {
    unsigned slot = 0;
    while (slot < PUBLISHED_IDS_MAX && published_ids[slot].id[0] &&
           strcmp(published_ids[slot].id, id) != 0)
        slot++;
    if (slot == PUBLISHED_IDS_MAX || strlen(id) >= sizeof(published_ids[slot].id)) return;
    if (!published_ids[slot].id[0]) strncpy(published_ids[slot].id, id, sizeof(published_ids[slot].id) - 1);
    unsigned instance = published_ids[slot].count++;
    if (instance > 99U) return;

    char name[24];
    unsigned used = 0;
    append(name, sizeof(name), &used, id);
    char number[4] = {':', (char)('0' + instance / 10U), (char)('0' + instance % 10U), '\0'};
    append(name, sizeof(name), &used, number);

    char modalias[128];
    used = 0;
    append(modalias, sizeof(modalias), &used, "acpi:");
    append(modalias, sizeof(modalias), &used, id);
    append(modalias, sizeof(modalias), &used, ":");
    for (uacpi_u32 index = 0; cids && index < cids->num_ids; index++) {
        append(modalias, sizeof(modalias), &used, cids->ids[index].value);
        append(modalias, sizeof(modalias), &used, ":");
    }

    char path[96];
    sysfs_publish_acpi_device(name, modalias, node ? acpi_node_path(node, path, sizeof(path)) : "\\");
}

static uacpi_iteration_decision publish_device(void *user, uacpi_namespace_node *node,
                                               uacpi_u32 depth) {
    (void)user;
    (void)depth;
    if (!acpi_device_present(node)) return UACPI_ITERATION_DECISION_CONTINUE;
    uacpi_id_string *hid = NULL;
    if (uacpi_eval_hid(node, &hid) == UACPI_STATUS_OK && hid) {
        uacpi_pnp_id_list *cids = NULL;
        if (uacpi_eval_cid(node, &cids) != UACPI_STATUS_OK) cids = NULL;
        publish(node, hid->value, cids);
        if (cids) uacpi_free_pnp_id_list(cids);
        uacpi_free_id_string(hid);
    }
    // Linux's name for a display adapter whose outputs have brightness controls.
    uacpi_namespace_node *method = NULL;
    uacpi_namespace_node *parent = uacpi_namespace_node_parent(node);
    if (parent && uacpi_namespace_node_find(node, "_BCM", &method) == UACPI_STATUS_OK)
        publish(parent, "LNXVIDEO", NULL);
    return UACPI_ITERATION_DECISION_CONTINUE;
}

static uacpi_iteration_decision publish_object(void *user, uacpi_namespace_node *node,
                                               uacpi_u32 depth) {
    (void)depth;
    publish(node, (const char *)user, NULL);
    return UACPI_ITERATION_DECISION_CONTINUE;
}

// What udev would be told about: every present device by its ID, and the objects
// Linux gives made-up IDs to, so that modprobe can load their drivers.
static void publish_devices(void) {
    struct acpi_fadt *fadt = NULL;
    if (uacpi_table_fadt(&fadt) == UACPI_STATUS_OK && fadt && !(fadt->flags & ACPI_PWR_BUTTON))
        publish(NULL, "LNXPWRBN", NULL);
    (void)uacpi_namespace_for_each_child(uacpi_namespace_root(), publish_object, NULL,
                                         UACPI_OBJECT_PROCESSOR_BIT, UACPI_MAX_DEPTH_ANY,
                                         (void *)"LNXCPU");
    (void)uacpi_namespace_for_each_child(uacpi_namespace_root(), publish_object, NULL,
                                         UACPI_OBJECT_THERMAL_ZONE_BIT, UACPI_MAX_DEPTH_ANY,
                                         (void *)"LNXTHERM");
    (void)uacpi_namespace_for_each_child(uacpi_namespace_root(), publish_device, NULL,
                                         UACPI_OBJECT_DEVICE_BIT, UACPI_MAX_DEPTH_ANY, NULL);
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
    (void)step("system notify handler",
               uacpi_install_notify_handler(uacpi_namespace_root(), system_notify, NULL));
    publish_devices();
    if (embedded) acpi_ec_start();
    (void)step("gpe setup", uacpi_finalize_gpe_initialization());
    kprintf("ACPI: ready in %u ms\n", (unsigned)((time_uptime_ns() - started) / 1000000ULL));
}

int acpi_subsystem_ready(void) { return subsystem_ready; }

void acpi_describe_subsystem(struct acpi_subsystem_info *out) {
    out->ready = subsystem_ready;
    out->embedded_controller = acpi_ec_present();
    out->ec_events = acpi_ec_events();
    out->thermal_zones = __atomic_load_n(&acpi_thermal_zone_count, __ATOMIC_RELAXED);
    out->video_outputs = __atomic_load_n(&acpi_video_output_count, __ATOMIC_RELAXED);
    out->notifications = __atomic_load_n(&notifications, __ATOMIC_RELAXED);
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
