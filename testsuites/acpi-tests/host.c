#include <setjmp.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include <tunix/acpi.h>
#include <tunix/boot.h>
#include <tunix/io.h>
#include <tunix/process.h>
#include <uacpi/kernel_api.h>
#include <uacpi/uacpi.h>
#include <uapi/input_event.h>

#define PHYSICAL_BYTES 0x100000000ULL
#define RSDP_ADDRESS   0xE0000ULL
#define XSDT_ADDRESS   0x100000ULL
#define FADT_ADDRESS   0x101000ULL
#define FACS_ADDRESS   0x102000ULL
#define DSDT_ADDRESS   0x110000ULL
#define FADT_BYTES     276U
#define PM1_EVENT      0x400U
#define PM1_CONTROL    0x404U
#define PM_TIMER       0x408U
#define GPE0_BLOCK     0x420U
#define GPE0_BYTES     16U
#define EC_DATA        0x62U
#define EC_COMMAND     0x66U
#define EC_GPE         0x16U
#define EC_LID         0x4EU
#define EC_TEMPERATURE 0x58U
#define EC_THROTTLE    0x90U
#define EC_HOTKEY      0xA3U
#define SCI_ENABLED    0x01U
#define POWER_BUTTON   0x01U
#define EVENTS_MAX     32U
#define KEYS_MAX       64U
#define THREADS_MAX    16U
#define WORK_MAX       256U
#define LOG_BYTES      8192U

struct ec_event {
    uint8_t query;
    uint8_t hotkey;
};

struct kthread {
    const char *name;
    void (*body)(void *);
    void *argument;
};

struct pending {
    uacpi_work_handler handler;
    uacpi_handle context;
};

static uint8_t *memory;
static uint8_t ports[0x10000];
static uint64_t clock_ns;
static int failures;

static uint8_t ec_ram[256];
static uint8_t ec_output;
static int ec_output_full;
static int ec_state;
static uint8_t ec_address;
static struct ec_event ec_queue[EVENTS_MAX];
static unsigned ec_head;
static unsigned ec_tail;
static unsigned ec_transfers;

static uint16_t keys[KEYS_MAX];
static unsigned key_count;
static int reader_present;
static int steps_up;
static int steps_down;
static int button_presses;
static int critical_requests;
static int passive_requested;
static uint64_t limit_khz;

static struct kthread threads[THREADS_MAX];
static unsigned thread_count;
static jmp_buf thread_exit;
static int thread_running;
static struct process boot_thread = {1};

static struct pending work[WORK_MAX];
static unsigned work_count;
static uacpi_interrupt_handler sci_handler;
static uacpi_handle sci_context;

static char log_text[LOG_BYTES];
static size_t log_used;

static struct boot_info boot;

void kprintf(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int wrote = vsnprintf(log_text + log_used, LOG_BYTES - log_used, fmt, args);
    va_end(args);
    if (wrote > 0) {
        fputs(log_text + log_used, stdout);
        log_used +=
            (size_t)wrote < LOG_BYTES - log_used ? (size_t)wrote : LOG_BYTES - log_used - 1U;
    }
}

static int logged(const char *text) { return strstr(log_text, text) != NULL; }

static uint8_t ec_status(void) {
    return (uint8_t)((ec_output_full ? 0x01U : 0U) | (ec_head != ec_tail ? 0x20U : 0U));
}

static void ec_command(uint8_t value) {
    if (value == 0x84U) {
        ec_output = 0;
        if (ec_head != ec_tail) {
            ec_output = ec_queue[ec_head].query;
            if (ec_queue[ec_head].hotkey) ec_ram[EC_HOTKEY] = ec_queue[ec_head].hotkey;
            ec_head++;
        }
        ec_output_full = 1;
    } else if (value == 0x80U) {
        ec_state = 1;
    } else if (value == 0x81U) {
        ec_state = 2;
    }
}

static void ec_data(uint8_t value) {
    if (ec_state == 1) {
        ec_output = ec_ram[value];
        ec_output_full = 1;
        ec_state = 0;
        ec_transfers++;
    } else if (ec_state == 2) {
        ec_address = value;
        ec_state = 3;
    } else if (ec_state == 3) {
        ec_ram[ec_address] = value;
        ec_state = 0;
        ec_transfers++;
    }
}

uint8_t inb(uint16_t port) {
    if (port == EC_COMMAND) return ec_status();
    if (port == EC_DATA) {
        ec_output_full = 0;
        return ec_output;
    }
    if (port == PM1_CONTROL) return (uint8_t)(ports[port] | SCI_ENABLED);
    return ports[port];
}

uint16_t inw(uint16_t port) { return (uint16_t)(inb(port) | (inb((uint16_t)(port + 1U)) << 8)); }

uint32_t inl(uint16_t port) {
    return (uint32_t)inw(port) | ((uint32_t)inw((uint16_t)(port + 2U)) << 16);
}

static int status_register(uint16_t port) {
    return (port >= GPE0_BLOCK && port < GPE0_BLOCK + GPE0_BYTES / 2U) || port == PM1_EVENT ||
        port == PM1_EVENT + 1U;
}

void outb(uint16_t port, uint8_t value) {
    if (port == EC_COMMAND) ec_command(value);
    else if (port == EC_DATA) ec_data(value);
    else if (status_register(port)) ports[port] &= (uint8_t)~value;
    else ports[port] = value;
}

void outw(uint16_t port, uint16_t value) {
    outb(port, (uint8_t)value);
    outb((uint16_t)(port + 1U), (uint8_t)(value >> 8));
}

void outl(uint16_t port, uint32_t value) {
    outw(port, (uint16_t)value);
    outw((uint16_t)(port + 2U), (uint16_t)(value >> 16));
}

static void run_work(void) {
    while (work_count) {
        struct pending item = work[0];
        memmove(work, work + 1, (work_count - 1U) * sizeof(work[0]));
        work_count--;
        item.handler(item.context);
    }
}

const struct boot_info *boot_info(void) { return &boot; }

const char *boot_command_line_value(const char *key) {
    (void)key;
    return NULL;
}

int boot_command_line_flag(const char *key) {
    (void)key;
    return 0;
}

uint64_t time_uptime_ns(void) { return clock_ns += 1000U; }

void process_prepare_wait(const void *channel, uint64_t deadline_ns) {
    (void)channel;
    (void)deadline_ns;
}

void process_wait(void) {
    if (thread_running) longjmp(thread_exit, 1);
}

void process_finish_wait(void) {}

int process_wake_all(const void *channel) {
    (void)channel;
    return 0;
}

struct process *process_create_kthread(const char *name, void (*body)(void *), void *argument) {
    if (thread_count < THREADS_MAX)
        threads[thread_count++] = (struct kthread){name, body, argument};
    return &boot_thread;
}

struct process *process_current(void) { return &boot_thread; }

static int run_thread_once(const char *name) {
    for (unsigned index = 0; index < thread_count; index++) {
        if (strcmp(threads[index].name, name) != 0) continue;
        thread_running = 1;
        if (!setjmp(thread_exit)) threads[index].body(threads[index].argument);
        thread_running = 0;
        run_work();
        return 0;
    }
    return -1;
}

int input_report_hotkey(uint16_t keycode) {
    if (key_count < KEYS_MAX) keys[key_count++] = keycode;
    return reader_present;
}

int backlight_step(int brighter) {
    if (brighter) steps_up++;
    else steps_down++;
    return 0;
}

void power_button_pressed(void) { button_presses++; }

void power_critical(void) { critical_requests++; }

void thermal_set_passive(int on) { passive_requested = on; }

void cpufreq_set_limit_khz(uint64_t khz) { limit_khz = khz; }

typedef int (*thermal_zone_reader)(unsigned index, int32_t *millicelsius);
static thermal_zone_reader zone_reader;

void sysfs_publish_thermal_zone(unsigned index, const char *type, thermal_zone_reader reader) {
    (void)index;
    (void)type;
    zone_reader = reader;
}

int acpi_host_start(void) { return 0; }

uacpi_status uacpi_kernel_get_rsdp(uacpi_phys_addr *out_rsdp_address) {
    *out_rsdp_address = RSDP_ADDRESS;
    return UACPI_STATUS_OK;
}

void *uacpi_kernel_map(uacpi_phys_addr addr, uacpi_size len) {
    if (addr + len > PHYSICAL_BYTES) return UACPI_MAP_FAILED;
    return memory + addr;
}

void uacpi_kernel_unmap(void *addr, uacpi_size len) {
    (void)addr;
    (void)len;
}

void uacpi_kernel_log(uacpi_log_level level, const uacpi_char *text) {
    if (level <= UACPI_LOG_WARN) kprintf("uacpi: %s", text);
}

uacpi_status uacpi_kernel_pci_device_open(uacpi_pci_address address, uacpi_handle *out_handle) {
    (void)address;
    *out_handle = &boot_thread;
    return UACPI_STATUS_OK;
}

void uacpi_kernel_pci_device_close(uacpi_handle handle) { (void)handle; }

uacpi_status uacpi_kernel_pci_read8(uacpi_handle device, uacpi_size offset, uacpi_u8 *value) {
    (void)device;
    (void)offset;
    *value = 0;
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_pci_read16(uacpi_handle device, uacpi_size offset, uacpi_u16 *value) {
    (void)device;
    (void)offset;
    *value = 0;
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_pci_read32(uacpi_handle device, uacpi_size offset, uacpi_u32 *value) {
    (void)device;
    (void)offset;
    *value = 0;
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_pci_write8(uacpi_handle device, uacpi_size offset, uacpi_u8 value) {
    (void)device;
    (void)offset;
    (void)value;
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_pci_write16(uacpi_handle device, uacpi_size offset, uacpi_u16 value) {
    (void)device;
    (void)offset;
    (void)value;
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_pci_write32(uacpi_handle device, uacpi_size offset, uacpi_u32 value) {
    (void)device;
    (void)offset;
    (void)value;
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_io_map(uacpi_io_addr base, uacpi_size len, uacpi_handle *out_handle) {
    (void)len;
    *out_handle = (uacpi_handle)(uintptr_t)(base + 1U);
    return UACPI_STATUS_OK;
}

void uacpi_kernel_io_unmap(uacpi_handle handle) { (void)handle; }

static uint16_t io_port(uacpi_handle handle, uacpi_size offset) {
    return (uint16_t)((uintptr_t)handle - 1U + offset);
}

uacpi_status uacpi_kernel_io_read8(uacpi_handle handle, uacpi_size offset, uacpi_u8 *value) {
    *value = inb(io_port(handle, offset));
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_io_read16(uacpi_handle handle, uacpi_size offset, uacpi_u16 *value) {
    *value = inw(io_port(handle, offset));
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_io_read32(uacpi_handle handle, uacpi_size offset, uacpi_u32 *value) {
    *value = inl(io_port(handle, offset));
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_io_write8(uacpi_handle handle, uacpi_size offset, uacpi_u8 value) {
    outb(io_port(handle, offset), value);
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_io_write16(uacpi_handle handle, uacpi_size offset, uacpi_u16 value) {
    outw(io_port(handle, offset), value);
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_io_write32(uacpi_handle handle, uacpi_size offset, uacpi_u32 value) {
    outl(io_port(handle, offset), value);
    return UACPI_STATUS_OK;
}

void *uacpi_kernel_alloc(uacpi_size size) { return malloc(size ? size : 1U); }

void uacpi_kernel_free(void *mem) { free(mem); }

uacpi_u64 uacpi_kernel_get_nanoseconds_since_boot(void) { return clock_ns += 1000U; }

void uacpi_kernel_stall(uacpi_u8 usec) { clock_ns += usec * 1000ULL; }

void uacpi_kernel_sleep(uacpi_u64 msec) { clock_ns += msec * 1000000ULL; }

uacpi_handle uacpi_kernel_create_mutex(void) { return calloc(1, sizeof(uint64_t)); }

void uacpi_kernel_free_mutex(uacpi_handle handle) { free(handle); }

uacpi_handle uacpi_kernel_create_event(void) { return calloc(1, sizeof(uint64_t)); }

void uacpi_kernel_free_event(uacpi_handle handle) { free(handle); }

uacpi_thread_id uacpi_kernel_get_thread_id(void) { return (uacpi_thread_id)&boot_thread; }

uacpi_status uacpi_kernel_acquire_mutex(uacpi_handle handle, uacpi_u16 timeout) {
    (void)handle;
    (void)timeout;
    return UACPI_STATUS_OK;
}

void uacpi_kernel_release_mutex(uacpi_handle handle) { (void)handle; }

uacpi_bool uacpi_kernel_wait_for_event(uacpi_handle handle, uacpi_u16 timeout) {
    (void)handle;
    (void)timeout;
    return UACPI_TRUE;
}

void uacpi_kernel_signal_event(uacpi_handle handle) { (void)handle; }

void uacpi_kernel_reset_event(uacpi_handle handle) { (void)handle; }

uacpi_status uacpi_kernel_handle_firmware_request(uacpi_firmware_request *request) {
    (void)request;
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_install_interrupt_handler(uacpi_u32 irq, uacpi_interrupt_handler handler,
                                                    uacpi_handle ctx,
                                                    uacpi_handle *out_irq_handle) {
    (void)irq;
    sci_handler = handler;
    sci_context = ctx;
    *out_irq_handle = &boot_thread;
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_uninstall_interrupt_handler(uacpi_interrupt_handler handler,
                                                      uacpi_handle irq_handle) {
    (void)handler;
    (void)irq_handle;
    return UACPI_STATUS_OK;
}

uacpi_handle uacpi_kernel_create_spinlock(void) { return calloc(1, sizeof(uint64_t)); }

void uacpi_kernel_free_spinlock(uacpi_handle handle) { free(handle); }

uacpi_cpu_flags uacpi_kernel_lock_spinlock(uacpi_handle handle) {
    (void)handle;
    return 0;
}

void uacpi_kernel_unlock_spinlock(uacpi_handle handle, uacpi_cpu_flags flags) {
    (void)handle;
    (void)flags;
}

uacpi_interrupt_state uacpi_kernel_disable_interrupts(void) { return 0; }

void uacpi_kernel_restore_interrupts(uacpi_interrupt_state state) { (void)state; }

uacpi_status uacpi_kernel_schedule_work(uacpi_work_type type, uacpi_work_handler handler,
                                        uacpi_handle ctx) {
    (void)type;
    if (work_count == WORK_MAX) return UACPI_STATUS_OUT_OF_MEMORY;
    work[work_count++] = (struct pending){handler, ctx};
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_wait_for_work_completion(void) {
    run_work();
    return UACPI_STATUS_OK;
}

static void checksum(uint8_t *table, uint32_t length, unsigned offset) {
    table[offset] = 0;
    uint8_t sum = 0;
    for (uint32_t index = 0; index < length; index++) sum = (uint8_t)(sum + table[index]);
    table[offset] = (uint8_t)-sum;
}

static void put32(uint8_t *at, uint32_t value) { memcpy(at, &value, sizeof(value)); }

static void put64(uint8_t *at, uint64_t value) { memcpy(at, &value, sizeof(value)); }

static void header(uint8_t *table, const char *signature, uint32_t length, uint8_t revision) {
    memcpy(table, signature, 4);
    put32(table + 4, length);
    table[8] = revision;
    memcpy(table + 10, "TUNIX ", 6);
    memcpy(table + 16, "MACHINE ", 8);
}

static int build_machine(const char *dsdt_path) {
    FILE *file = fopen(dsdt_path, "rb");
    if (!file) {
        perror(dsdt_path);
        return -1;
    }
    size_t dsdt_bytes = fread(memory + DSDT_ADDRESS, 1, 0x10000, file);
    fclose(file);
    if (dsdt_bytes < 36U) return -1;

    uint8_t *facs = memory + FACS_ADDRESS;
    memcpy(facs, "FACS", 4);
    put32(facs + 4, 64);

    uint8_t *fadt = memory + FADT_ADDRESS;
    header(fadt, "FACP", FADT_BYTES, 6);
    put32(fadt + 36, (uint32_t)FACS_ADDRESS);
    put32(fadt + 40, (uint32_t)DSDT_ADDRESS);
    fadt[46] = 9;
    put32(fadt + 48, 0xB2);
    fadt[52] = 0xA0;
    fadt[53] = 0xA1;
    put32(fadt + 56, PM1_EVENT);
    put32(fadt + 64, PM1_CONTROL);
    put32(fadt + 76, PM_TIMER);
    put32(fadt + 80, GPE0_BLOCK);
    fadt[88] = 4;
    fadt[89] = 2;
    fadt[91] = 4;
    fadt[92] = GPE0_BYTES;
    put64(fadt + 132, FACS_ADDRESS);
    put64(fadt + 140, DSDT_ADDRESS);
    checksum(fadt, FADT_BYTES, 9);

    uint8_t *xsdt = memory + XSDT_ADDRESS;
    header(xsdt, "XSDT", 44, 1);
    put64(xsdt + 36, FADT_ADDRESS);
    checksum(xsdt, 44, 9);

    uint8_t *rsdp = memory + RSDP_ADDRESS;
    memcpy(rsdp, "RSD PTR ", 8);
    memcpy(rsdp + 9, "TUNIX ", 6);
    rsdp[15] = 2;
    put32(rsdp + 20, 36);
    put64(rsdp + 24, XSDT_ADDRESS);
    checksum(rsdp, 20, 8);
    checksum(rsdp, 36, 32);
    boot.rsdp = RSDP_ADDRESS;
    return 0;
}

static void expect(const char *what, long got, long want) {
    printf("%-58s %s", what, got == want ? "ok\n" : "FAIL");
    if (got != want) {
        printf(" (got %ld, want %ld)\n", got, want);
        failures++;
    }
}

static void raise_ec_event(uint8_t query, uint8_t hotkey) {
    ec_queue[ec_tail++ % EVENTS_MAX] = (struct ec_event){query, hotkey};
    ports[GPE0_BLOCK + EC_GPE / 8U] |= (uint8_t)(1U << (EC_GPE % 8U));
    if (sci_handler) sci_handler(sci_context);
    run_work();
}

static uint64_t named_integer(const char *path) {
    uint64_t value = 0;
    if (uacpi_eval_simple_integer(NULL, path, &value) != UACPI_STATUS_OK) return UINT64_MAX;
    return (long)value;
}

static void check_init(void) {
    struct acpi_subsystem_info info;
    acpi_describe_subsystem(&info);
    expect("the aml subsystem comes up", info.ready, 1);
    expect("the embedded controller is found by its hid", info.embedded_controller, 1);
    expect("_REG connected the ec region", (long)named_integer("\\ECON"), 1);
    expect("the sci handler is installed", sci_handler != NULL, 1);
    expect("the ec gpe is enabled",
           (ports[GPE0_BLOCK + GPE0_BYTES / 2U + EC_GPE / 8U] >> (EC_GPE % 8U)) & 1U, 1);
    expect("both display outputs are found", info.video_outputs, 2);
    expect("_DOS tells the firmware the os does brightness", (long)named_integer("\\DOSA"), 4);
    expect("_PDC declares ffh p-states", (long)named_integer("\\PDCC"), 0x0B);
    expect("the thermal zone is found", info.thermal_zones, 1);
}

static void check_brightness_keys(void) {
    unsigned before = key_count;
    raise_ec_event(0x60, 0x04);
    expect("hotkey 4 gives one key although two outputs hear it", key_count - before, 1);
    expect("hotkey 4 is brightness down", keys[key_count - 1U], TUNIX_KEY_BRIGHTNESSDOWN);
    expect("with no reader the kernel steps the panel down", steps_down, 1);
    clock_ns += 100000000ULL;
    before = key_count;
    raise_ec_event(0x60, 0x05);
    expect("hotkey 5 is brightness up",
           key_count - before == 1 && keys[key_count - 1U] == TUNIX_KEY_BRIGHTNESSUP, 1);
    clock_ns += 100000000ULL;
    reader_present = 1;
    int steps = steps_up + steps_down;
    raise_ec_event(0x60, 0x05);
    expect("with a reader the kernel leaves the panel alone", steps_up + steps_down, steps);
    before = key_count;
    raise_ec_event(0x60, 0x0F);
    expect("an unrelated hotkey gives no key", key_count - before, 0);
    raise_ec_event(0x55, 0);
    expect("an event with no method is reported", logged("ec event 55 has no _Q55 method"), 1);
    expect("the ec queue is empty", ec_head == ec_tail, 1);
    struct acpi_subsystem_info info;
    acpi_describe_subsystem(&info);
    expect("notifications nobody claims are counted", info.notifications > 0, 1);
    expect("and do not warn", logged("no listeners"), 0);
}

static void check_processor_limit(void) {
    expect("no processor limit at first", (long)limit_khz, 0);
    ec_ram[EC_THROTTLE] = 2;
    raise_ec_event(0x8E, 0);
    expect("the ec asks for state 2: 1200 MHz", (long)limit_khz, 1200000);
    ec_ram[EC_THROTTLE] = 1;
    raise_ec_event(0x8E, 0);
    expect("state 1: 1600 MHz", (long)limit_khz, 1600000);
    ec_ram[EC_THROTTLE] = 0;
    raise_ec_event(0x8E, 0);
    expect("state 0 lifts the limit", (long)limit_khz, 0);
}

static void check_thermal_zone(void) {
    int32_t millicelsius = 0;
    ec_ram[EC_TEMPERATURE] = 45;
    raise_ec_event(0x80, 0);
    run_thread_once("kacpi-thermal");
    expect("the zone reads the ec temperature",
           zone_reader && zone_reader(0, &millicelsius) == 0 && millicelsius == 45000, 1);
    expect("no passive cooling at 45 C", passive_requested, 0);
    ec_ram[EC_TEMPERATURE] = 97;
    run_thread_once("kacpi-thermal");
    expect("passive cooling above _PSV", passive_requested, 1);
    ec_ram[EC_TEMPERATURE] = 92;
    run_thread_once("kacpi-thermal");
    expect("still cooling within 5 C of _PSV", passive_requested, 1);
    ec_ram[EC_TEMPERATURE] = 85;
    run_thread_once("kacpi-thermal");
    expect("cooling stops 5 C under _PSV", passive_requested, 0);
    expect("no power off below _CRT", critical_requests, 0);
    ec_ram[EC_TEMPERATURE] = 106;
    run_thread_once("kacpi-thermal");
    expect("an orderly power off at _CRT", critical_requests, 1);
}

static void check_buttons_and_lid(void) {
    ec_ram[EC_LID] = 0;
    raise_ec_event(0x8A, 0);
    expect("the lid reports closed", logged("ACPI: lid closed"), 1);
    ports[PM1_EVENT + 1U] |= POWER_BUTTON;
    ports[PM1_EVENT + 3U] |= POWER_BUTTON;
    if (sci_handler) sci_handler(sci_context);
    run_work();
    expect("the fixed power button reaches the kernel", button_presses, 1);
}

static void check_poller(void) {
    ec_queue[ec_tail++ % EVENTS_MAX] = (struct ec_event){0x60, 0x04};
    unsigned before = key_count;
    clock_ns += 100000000ULL;
    run_thread_once("kacpi-ec");
    expect("an event without a gpe edge is drained by the poller", key_count - before, 1);
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s DSDT.aml\n", argv[0]);
        return 2;
    }
    memory = mmap(NULL, PHYSICAL_BYTES, PROT_READ | PROT_WRITE,
                  MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (memory == MAP_FAILED || build_machine(argv[1]) != 0) return 2;
    acpi_subsystem_init();
    run_work();
    check_init();
    check_brightness_keys();
    check_processor_limit();
    check_thermal_zone();
    check_buttons_and_lid();
    check_poller();
    printf(failures ? "acpi: FAIL %d\n" : "acpi: PASS\n", failures);
    return failures != 0;
}
