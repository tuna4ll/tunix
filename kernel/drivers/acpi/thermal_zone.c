#include <stddef.h>
#include <stdint.h>

#include <tunix/power.h>
#include <tunix/process.h>
#include <tunix/sysfs.h>
#include <tunix/thermal.h>
#include <tunix/time.h>
#include <uacpi/namespace.h>
#include <uacpi/notify.h>
#include <uacpi/types.h>
#include <uacpi/uacpi.h>

#include "priv.h"

extern void kprintf(const char *fmt, ...);

#define ZONES_MAX            8U
#define POLL_NS              5000000000ULL
#define KELVIN_OFFSET_DECI   2732
#define PASSIVE_RELEASE_DECI 50
#define NOTIFY_TEMPERATURE   0x80U
#define NOTIFY_TRIP_POINTS   0x81U

struct zone {
    uacpi_namespace_node *node;
    int32_t temperature;
    int32_t critical;
    int32_t hot;
    int32_t passive;
    uint8_t passive_on;
    uint8_t readable;
    volatile uint32_t trips_stale;
};

static struct zone zones[ZONES_MAX];
static unsigned zone_count;
static const char zone_channel;
static int critical_reported;

static int32_t celsius_tenths(int32_t deci_kelvin) { return deci_kelvin - KELVIN_OFFSET_DECI; }

static int32_t read_trip(uacpi_namespace_node *node, const char *method) {
    uint64_t value = 0;
    if (uacpi_eval_simple_integer(node, method, &value) != UACPI_STATUS_OK) return 0;
    return value > 0 && value < 0x10000U ? (int32_t)value : 0;
}

static void read_trips(struct zone *zone) {
    zone->critical = read_trip(zone->node, "_CRT");
    zone->hot = read_trip(zone->node, "_HOT");
    zone->passive = read_trip(zone->node, "_PSV");
}

static int read_temperature(struct zone *zone) {
    uint64_t value = 0;
    if (uacpi_eval_simple_integer(zone->node, "_TMP", &value) != UACPI_STATUS_OK || !value ||
        value >= 0x10000U) {
        zone->readable = 0;
        return -1;
    }
    zone->temperature = (int32_t)value;
    zone->readable = 1;
    return 0;
}

static void check(struct zone *zone, unsigned index) {
    if (__atomic_exchange_n(&zone->trips_stale, 0U, __ATOMIC_ACQ_REL)) read_trips(zone);
    if (read_temperature(zone) != 0) return;
    int32_t now = zone->temperature;
    if (zone->critical && now >= zone->critical && !critical_reported) {
        critical_reported = 1;
        kprintf("ACPI: thermal zone %u at %d.%d C reached its critical %d C\n", index,
                celsius_tenths(now) / 10, celsius_tenths(now) % 10,
                celsius_tenths(zone->critical) / 10);
        power_critical();
    }
    if (zone->passive && !zone->passive_on && now >= zone->passive) {
        zone->passive_on = 1;
        kprintf("ACPI: thermal zone %u at %d C, passive cooling on\n", index,
                celsius_tenths(now) / 10);
    } else if (zone->passive_on && now + PASSIVE_RELEASE_DECI < zone->passive) {
        zone->passive_on = 0;
        kprintf("ACPI: thermal zone %u at %d C, passive cooling off\n", index,
                celsius_tenths(now) / 10);
    }
}

static void check_all(void) {
    int passive = 0;
    for (unsigned index = 0; index < zone_count; index++) {
        check(&zones[index], index);
        passive |= zones[index].passive_on;
    }
    thermal_set_passive(passive);
}

static void watcher(void *unused) {
    (void)unused;
    for (;;) {
        check_all();
        process_prepare_wait(&zone_channel, time_uptime_ns() + POLL_NS);
        process_wait();
        process_finish_wait();
    }
}

static uacpi_status zone_notify(uacpi_handle context, uacpi_namespace_node *node, uacpi_u64 value) {
    (void)node;
    struct zone *zone = context;
    if (value == NOTIFY_TRIP_POINTS) __atomic_store_n(&zone->trips_stale, 1U, __ATOMIC_RELEASE);
    if (value == NOTIFY_TEMPERATURE || value == NOTIFY_TRIP_POINTS)
        (void)process_wake_all(&zone_channel);
    return UACPI_STATUS_OK;
}

static int zone_millicelsius(unsigned index, int32_t *out) {
    if (index >= zone_count || !zones[index].readable) return -1;
    *out = celsius_tenths(zones[index].temperature) * 100;
    return 0;
}

static uacpi_iteration_decision add_zone(void *user, uacpi_namespace_node *node, uacpi_u32 depth) {
    (void)user;
    (void)depth;
    if (zone_count == ZONES_MAX) return UACPI_ITERATION_DECISION_BREAK;
    struct zone *zone = &zones[zone_count];
    zone->node = node;
    read_trips(zone);
    (void)read_temperature(zone);
    char path[64];
    if (uacpi_install_notify_handler(node, zone_notify, zone) != UACPI_STATUS_OK)
        kprintf("ACPI: no notify handler on %s\n", acpi_node_path(node, path, sizeof(path)));
    kprintf("ACPI: thermal zone %u %s at %d C, passive %d C, critical %d C\n", zone_count,
            acpi_node_path(node, path, sizeof(path)),
            zone->readable ? celsius_tenths(zone->temperature) / 10 : -1,
            zone->passive ? celsius_tenths(zone->passive) / 10 : -1,
            zone->critical ? celsius_tenths(zone->critical) / 10 : -1);
    sysfs_publish_thermal_zone(zone_count, "acpitz", zone_millicelsius);
    zone_count++;
    return UACPI_ITERATION_DECISION_CONTINUE;
}

void acpi_thermal_probe(void) {
    (void)uacpi_namespace_for_each_child(uacpi_namespace_root(), add_zone, NULL,
                                         UACPI_OBJECT_THERMAL_ZONE_BIT, UACPI_MAX_DEPTH_ANY, NULL);
    if (!zone_count) return;
    if (!process_create_kthread("kacpi-thermal", watcher, NULL))
        kprintf("ACPI: cannot watch the thermal zones\n");
}

unsigned acpi_thermal_zones(void) { return zone_count; }
