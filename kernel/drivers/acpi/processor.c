#include <stddef.h>
#include <stdint.h>

#include <tunix/cpufreq.h>
#include <uacpi/namespace.h>
#include <uacpi/notify.h>
#include <uacpi/types.h>
#include <uacpi/uacpi.h>
#include <uacpi/utilities.h>

#include "priv.h"

extern void kprintf(const char *fmt, ...);

#define PROCESSORS_MAX            64U
#define PDC_REVISION              1U
#define PDC_PERFORMANCE_FFH       0x0001U
#define PDC_C1_HALT               0x0002U
#define PDC_SMP_C1_INDEPENDENT    0x0008U
#define PDC_CAPABILITIES          (PDC_PERFORMANCE_FFH | PDC_C1_HALT | PDC_SMP_C1_INDEPENDENT)
#define NOTIFY_PERFORMANCE_CHANGE 0x80U

static uacpi_namespace_node *processors[PROCESSORS_MAX];
static unsigned processor_count;
static uacpi_namespace_node *performance_node;
static uint64_t applied_khz = UINT64_MAX;

static void declare_capabilities(uacpi_namespace_node *node) {
    uacpi_namespace_node *method = NULL;
    if (uacpi_namespace_node_find(node, "_PDC", &method) != UACPI_STATUS_OK) return;
    uint32_t words[3] = {PDC_REVISION, 1U, PDC_CAPABILITIES};
    uacpi_data_view view;
    view.const_data = words;
    view.length = sizeof(words);
    uacpi_object *buffer = uacpi_object_create_buffer(view);
    if (!buffer) return;
    uacpi_object_array arguments = {&buffer, 1};
    uacpi_status status = uacpi_execute(node, "_PDC", &arguments);
    uacpi_object_unref(buffer);
    if (status != UACPI_STATUS_OK) {
        char path[64];
        kprintf("ACPI: %s._PDC failed: %s\n", acpi_node_path(node, path, sizeof(path)),
                uacpi_status_to_string(status));
    }
}

static int state_khz(uacpi_namespace_node *node, uint64_t index, uint64_t *khz) {
    uacpi_object *table = NULL;
    if (uacpi_eval_simple_package(node, "_PSS", &table) != UACPI_STATUS_OK) return -1;
    uacpi_object_array states;
    int found = -1;
    if (uacpi_object_get_package(table, &states) == UACPI_STATUS_OK && index < states.count) {
        uacpi_object_array fields;
        uint64_t megahertz = 0;
        if (uacpi_object_get_package(states.objects[index], &fields) == UACPI_STATUS_OK &&
            fields.count >= 1 &&
            uacpi_object_get_integer(fields.objects[0], &megahertz) == UACPI_STATUS_OK) {
            *khz = megahertz * 1000ULL;
            found = 0;
        }
    }
    uacpi_object_unref(table);
    return found;
}

static void apply_performance_limit(void) {
    if (!performance_node) return;
    uint64_t index = 0;
    if (uacpi_eval_simple_integer(performance_node, "_PPC", &index) != UACPI_STATUS_OK) index = 0;
    uint64_t khz = 0;
    if (index && state_khz(performance_node, index, &khz) != 0) khz = 0;
    if (khz == applied_khz) return;
    applied_khz = khz;
    cpufreq_set_limit_khz(khz);
    if (khz) kprintf("ACPI: firmware limits the processors to %u MHz\n", (unsigned)(khz / 1000U));
    else kprintf("ACPI: no firmware limit on the processors\n");
}

static uacpi_status processor_notify(uacpi_handle context, uacpi_namespace_node *node,
                                     uacpi_u64 value) {
    (void)context;
    (void)node;
    if (value == NOTIFY_PERFORMANCE_CHANGE) apply_performance_limit();
    return UACPI_STATUS_OK;
}

static uacpi_iteration_decision add_processor(void *user, uacpi_namespace_node *node,
                                              uacpi_u32 depth) {
    (void)user;
    (void)depth;
    if (processor_count == PROCESSORS_MAX) return UACPI_ITERATION_DECISION_BREAK;
    for (unsigned index = 0; index < processor_count; index++)
        if (processors[index] == node) return UACPI_ITERATION_DECISION_CONTINUE;
    processors[processor_count++] = node;
    return UACPI_ITERATION_DECISION_CONTINUE;
}

void acpi_processor_probe(void) {
    (void)uacpi_namespace_for_each_child(uacpi_namespace_root(), add_processor, NULL,
                                         UACPI_OBJECT_PROCESSOR_BIT, UACPI_MAX_DEPTH_ANY, NULL);
    (void)uacpi_find_devices("ACPI0007", add_processor, NULL);
    for (unsigned index = 0; index < processor_count; index++) {
        uacpi_namespace_node *node = processors[index];
        declare_capabilities(node);
        (void)uacpi_install_notify_handler(node, processor_notify, NULL);
        uint64_t khz = 0;
        if (!performance_node && state_khz(node, 0, &khz) == 0) performance_node = node;
    }
    if (!processor_count) return;
    char path[64];
    kprintf("ACPI: %u processor(s), performance states %s\n", processor_count,
            performance_node ? acpi_node_path(performance_node, path, sizeof(path)) : "none");
    apply_performance_limit();
}
