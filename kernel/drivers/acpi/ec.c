#include <stddef.h>
#include <stdint.h>

#include <tunix/io.h>
#include <tunix/process.h>
#include <tunix/time.h>
#include <uacpi/event.h>
#include <uacpi/kernel_api.h>
#include <uacpi/namespace.h>
#include <uacpi/opregion.h>
#include <uacpi/resources.h>
#include <uacpi/uacpi.h>
#include <uacpi/utilities.h>

#include "priv.h"

extern void kprintf(const char *fmt, ...);

#define EC_OUTPUT_FULL  0x01U
#define EC_INPUT_FULL   0x02U
#define EC_SCI_EVENT    0x20U
#define EC_READ         0x80U
#define EC_WRITE        0x81U
#define EC_QUERY        0x84U
#define EC_WAIT_NS      500000000ULL
#define EC_POLL_NS      250000000ULL
#define EC_QUERIES_MAX  32U
#define EC_LOCK_FOREVER 0xFFFFU
#define EC_NO_GPE       0xFFFFU
#define EC_PORTS        2U

struct embedded_controller {
    uacpi_namespace_node *node;
    uint16_t ports[EC_PORTS];
    unsigned port_count;
    uint16_t gpe;
    int global_lock;
    uacpi_handle mutex;
    volatile uint32_t query_queued;
    volatile uint64_t events;
    int started;
};

static struct embedded_controller ec = {.gpe = EC_NO_GPE};
static const char poll_channel;

static uint16_t data_port(void) { return ec.ports[0]; }

static uint16_t command_port(void) { return ec.ports[1]; }

static int wait_status(uint8_t mask, uint8_t want) {
    uint64_t deadline = time_uptime_ns() + EC_WAIT_NS;
    while ((inb(command_port()) & mask) != want)
        if (time_uptime_ns() > deadline) return -1;
    return 0;
}

static int lock_ec(uint32_t *sequence) {
    if (uacpi_kernel_acquire_mutex(ec.mutex, EC_LOCK_FOREVER) != UACPI_STATUS_OK) return -1;
    if (ec.global_lock && uacpi_acquire_global_lock(EC_LOCK_FOREVER, sequence) != UACPI_STATUS_OK) {
        uacpi_kernel_release_mutex(ec.mutex);
        return -1;
    }
    return 0;
}

static void unlock_ec(uint32_t sequence) {
    if (ec.global_lock) (void)uacpi_release_global_lock(sequence);
    uacpi_kernel_release_mutex(ec.mutex);
}

static int read_locked(uint8_t address, uint8_t *value) {
    if (wait_status(EC_INPUT_FULL, 0) != 0) return -1;
    outb(command_port(), EC_READ);
    if (wait_status(EC_INPUT_FULL, 0) != 0) return -1;
    outb(data_port(), address);
    if (wait_status(EC_OUTPUT_FULL, EC_OUTPUT_FULL) != 0) return -1;
    *value = inb(data_port());
    return 0;
}

static int write_locked(uint8_t address, uint8_t value) {
    if (wait_status(EC_INPUT_FULL, 0) != 0) return -1;
    outb(command_port(), EC_WRITE);
    if (wait_status(EC_INPUT_FULL, 0) != 0) return -1;
    outb(data_port(), address);
    if (wait_status(EC_INPUT_FULL, 0) != 0) return -1;
    outb(data_port(), value);
    return wait_status(EC_INPUT_FULL, 0);
}

static int query_locked(uint8_t *query) {
    if (wait_status(EC_INPUT_FULL, 0) != 0) return -1;
    outb(command_port(), EC_QUERY);
    if (wait_status(EC_OUTPUT_FULL, EC_OUTPUT_FULL) != 0) return -1;
    *query = inb(data_port());
    return 0;
}

static uacpi_status transfer(uacpi_region_op op, uacpi_region_rw_data *rw) {
    uint32_t sequence = 0;
    if (rw->offset + rw->byte_width > 0x100U) return UACPI_STATUS_INVALID_ARGUMENT;
    if (lock_ec(&sequence) != 0) return UACPI_STATUS_TIMEOUT;
    int failed = 0;
    uint64_t value = op == UACPI_REGION_OP_READ ? 0 : rw->value;
    for (unsigned index = 0; index < rw->byte_width && !failed; index++) {
        uint8_t address = (uint8_t)(rw->offset + index);
        if (op == UACPI_REGION_OP_READ) {
            uint8_t byte = 0;
            failed = read_locked(address, &byte) != 0;
            value |= (uint64_t)byte << (index * 8U);
        } else {
            failed = write_locked(address, (uint8_t)(value >> (index * 8U))) != 0;
        }
    }
    unlock_ec(sequence);
    if (failed) return UACPI_STATUS_HARDWARE_TIMEOUT;
    if (op == UACPI_REGION_OP_READ) rw->value = value;
    return UACPI_STATUS_OK;
}

static uacpi_status region_handler(uacpi_region_op op, uacpi_handle data) {
    switch (op) {
    case UACPI_REGION_OP_ATTACH:
    case UACPI_REGION_OP_DETACH: return UACPI_STATUS_OK;
    case UACPI_REGION_OP_READ:
    case UACPI_REGION_OP_WRITE:  return transfer(op, (uacpi_region_rw_data *)data);
    default:                     return UACPI_STATUS_INVALID_ARGUMENT;
    }
}

static void run_query(uint8_t query) {
    static const char hex[] = "0123456789ABCDEF";
    char method[5] = {'_', 'Q', hex[query >> 4], hex[query & 0xFU], '\0'};
    __atomic_add_fetch(&ec.events, 1U, __ATOMIC_RELAXED);
    uacpi_namespace_node *target = NULL;
    if (uacpi_namespace_node_find(ec.node, method, &target) != UACPI_STATUS_OK) {
        kprintf("ACPI: ec event %x has no %s method\n", query, method);
        return;
    }
    uacpi_status status = uacpi_execute(ec.node, method, NULL);
    if (status != UACPI_STATUS_OK)
        kprintf("ACPI: ec %s failed: %s\n", method, uacpi_status_to_string(status));
}

static void drain_queries(uacpi_handle unused) {
    (void)unused;
    __atomic_store_n(&ec.query_queued, 0U, __ATOMIC_RELEASE);
    for (unsigned count = 0; count < EC_QUERIES_MAX; count++) {
        if (!(inb(command_port()) & EC_SCI_EVENT)) return;
        uint32_t sequence = 0;
        uint8_t query = 0;
        if (lock_ec(&sequence) != 0) return;
        int failed = query_locked(&query) != 0;
        unlock_ec(sequence);
        if (failed || !query) return;
        run_query(query);
    }
}

static void queue_queries(void) {
    if (__atomic_exchange_n(&ec.query_queued, 1U, __ATOMIC_ACQ_REL)) return;
    if (uacpi_kernel_schedule_work(UACPI_WORK_GPE_EXECUTION, drain_queries, NULL) !=
        UACPI_STATUS_OK)
        __atomic_store_n(&ec.query_queued, 0U, __ATOMIC_RELEASE);
}

static uacpi_interrupt_ret gpe_handler(uacpi_handle context, uacpi_namespace_node *device,
                                       uacpi_u16 index) {
    (void)context;
    (void)device;
    (void)index;
    if (inb(command_port()) & EC_SCI_EVENT) queue_queries();
    return UACPI_INTERRUPT_HANDLED | UACPI_GPE_REENABLE;
}

static void poller(void *unused) {
    (void)unused;
    for (;;) {
        if (inb(command_port()) & EC_SCI_EVENT) queue_queries();
        process_prepare_wait(&poll_channel, time_uptime_ns() + EC_POLL_NS);
        process_wait();
        process_finish_wait();
    }
}

static uacpi_iteration_decision take_port(void *user, uacpi_resource *resource) {
    (void)user;
    uint16_t port = 0;
    if (resource->type == UACPI_RESOURCE_TYPE_IO) port = resource->io.minimum;
    else if (resource->type == UACPI_RESOURCE_TYPE_FIXED_IO) port = resource->fixed_io.address;
    else return UACPI_ITERATION_DECISION_CONTINUE;
    if (ec.port_count < EC_PORTS) ec.ports[ec.port_count++] = port;
    return ec.port_count == EC_PORTS ? UACPI_ITERATION_DECISION_BREAK
                                     : UACPI_ITERATION_DECISION_CONTINUE;
}

static uacpi_iteration_decision take_device(void *user, uacpi_namespace_node *node,
                                            uacpi_u32 depth) {
    (void)user;
    (void)depth;
    ec.node = node;
    return UACPI_ITERATION_DECISION_BREAK;
}

int acpi_ec_probe(void) {
    if (uacpi_find_devices("PNP0C09", take_device, NULL) != UACPI_STATUS_OK || !ec.node) return -1;
    char path[64];
    if (uacpi_for_each_device_resource(ec.node, "_CRS", take_port, NULL) != UACPI_STATUS_OK ||
        ec.port_count != EC_PORTS) {
        kprintf("ACPI: ec %s has no usable ports\n", acpi_node_path(ec.node, path, sizeof(path)));
        ec.node = NULL;
        return -1;
    }
    uint64_t value = 0;
    if (uacpi_eval_simple_integer(ec.node, "_GPE", &value) == UACPI_STATUS_OK)
        ec.gpe = (uint16_t)value;
    if (uacpi_eval_simple_integer(ec.node, "_GLK", &value) == UACPI_STATUS_OK)
        ec.global_lock = value != 0;
    ec.mutex = uacpi_kernel_create_mutex();
    if (!ec.mutex) return -1;
    uacpi_status status = uacpi_install_address_space_handler(
        ec.node, UACPI_ADDRESS_SPACE_EMBEDDED_CONTROLLER, region_handler, NULL);
    if (status != UACPI_STATUS_OK) {
        kprintf("ACPI: ec region handler: %s\n", uacpi_status_to_string(status));
        ec.node = NULL;
        return -1;
    }
    kprintf("ACPI: ec %s at %x/%x, gpe %x\n", acpi_node_path(ec.node, path, sizeof(path)),
            data_port(), command_port(), ec.gpe);
    return 0;
}

void acpi_ec_start(void) {
    if (!ec.node || ec.started) return;
    ec.started = 1;
    if (ec.gpe != EC_NO_GPE) {
        uacpi_status status =
            uacpi_install_gpe_handler(NULL, ec.gpe, UACPI_GPE_TRIGGERING_EDGE, gpe_handler, NULL);
        if (status == UACPI_STATUS_OK) status = uacpi_enable_gpe(NULL, ec.gpe);
        if (status != UACPI_STATUS_OK)
            kprintf("ACPI: ec gpe %x: %s\n", ec.gpe, uacpi_status_to_string(status));
    }
    if (!process_create_kthread("kacpi-ec", poller, NULL))
        kprintf("ACPI: cannot start the ec poller\n");
    queue_queries();
}

int acpi_ec_present(void) { return ec.node != NULL; }

uint64_t acpi_ec_events(void) { return __atomic_load_n(&ec.events, __ATOMIC_RELAXED); }
