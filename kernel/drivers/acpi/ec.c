#include <stddef.h>
#include <stdint.h>

#include <tunix/acpi.h>
#include <tunix/backlight.h>
#include <tunix/boot.h>
#include <tunix/ec.h>
#include <tunix/input.h>
#include <tunix/io.h>
#include <tunix/kstring.h>
#include <tunix/process.h>
#include <tunix/time.h>
#include <uapi/input_event.h>

extern void kprintf(const char *fmt, ...);

#if defined(__x86_64__)

#define EC_DATA_PORT    0x62U
#define EC_COMMAND_PORT 0x66U
#define EC_OUTPUT_FULL  0x01U
#define EC_INPUT_FULL   0x02U
#define EC_SCI_EVENT    0x20U
#define EC_READ         0x80U
#define EC_QUERY        0x84U
#define EC_WAIT_NS      50000000ULL
#define EC_POLL_NS      50000000ULL
#define EC_DRAIN_MAX    8U
#define EC_LOG_REPEATS  3U

#define DSDT_OEM_ID       10U
#define DSDT_OEM_ID_BYTES 6U
#define DSDT_TABLE_ID     16U
#define DSDT_TABLE_BYTES  8U

struct ec_hotkey {
    uint8_t query;
    uint8_t reads_ram;
    uint8_t ram;
    uint8_t value;
    uint16_t keycode;
};

struct ec_board {
    const char *oem;
    const char *table;
    const struct ec_hotkey *keys;
    unsigned count;
};

static const struct ec_hotkey casper_era_keys[] = {
    {0x10, 0, 0, 0, TUNIX_KEY_BRIGHTNESSDOWN},
    {0x11, 0, 0, 0, TUNIX_KEY_BRIGHTNESSUP},
    {0x60, 1, 0xA3, 0x04, TUNIX_KEY_BRIGHTNESSDOWN},
    {0x60, 1, 0xA3, 0x05, TUNIX_KEY_BRIGHTNESSUP},
};

static const struct ec_board boards[] = {
    {"CASPER", "ERA", casper_era_keys, sizeof(casper_era_keys) / sizeof(casper_era_keys[0])},
};

static const struct ec_board *board;
static uint8_t query_seen[256];
static const char ec_channel;

static int wait_status(uint8_t mask, uint8_t want) {
    uint64_t deadline = time_uptime_ns() + EC_WAIT_NS;
    while ((inb(EC_COMMAND_PORT) & mask) != want)
        if (time_uptime_ns() > deadline) return -1;
    return 0;
}

static int send_command(uint8_t command) {
    if (wait_status(EC_INPUT_FULL, 0) != 0) return -1;
    outb(EC_COMMAND_PORT, command);
    return 0;
}

static int read_data(uint8_t *value) {
    if (wait_status(EC_OUTPUT_FULL, EC_OUTPUT_FULL) != 0) return -1;
    *value = inb(EC_DATA_PORT);
    return 0;
}

static int ec_query(uint8_t *query) {
    if (send_command(EC_QUERY) != 0) return -1;
    return read_data(query);
}

static int ec_read(uint8_t address, uint8_t *value) {
    if (send_command(EC_READ) != 0) return -1;
    if (wait_status(EC_INPUT_FULL, 0) != 0) return -1;
    outb(EC_DATA_PORT, address);
    return read_data(value);
}

static uint16_t hotkey_for(uint8_t query, uint8_t *ram_out, int *ram_read) {
    for (unsigned index = 0; index < board->count; index++) {
        const struct ec_hotkey *key = &board->keys[index];
        if (key->query != query) continue;
        if (!key->reads_ram) return key->keycode;
        if (!*ram_read) {
            if (ec_read(key->ram, ram_out) != 0) return 0;
            *ram_read = 1;
        }
        if (*ram_out == key->value) return key->keycode;
    }
    return 0;
}

static void handle_query(uint8_t query) {
    uint8_t ram = 0;
    int ram_read = 0;
    uint16_t keycode = hotkey_for(query, &ram, &ram_read);
    if (query_seen[query] < EC_LOG_REPEATS || boot_command_line_flag("hwreport")) {
        if (query_seen[query] < 255U) query_seen[query]++;
        if (ram_read) kprintf("EC: query %x code %x key %u\n", query, ram, keycode);
        else kprintf("EC: query %x key %u\n", query, keycode);
    }
    if (!keycode) return;
    if (input_report_hotkey(keycode)) return;
    if (keycode == TUNIX_KEY_BRIGHTNESSUP) (void)backlight_step(1);
    if (keycode == TUNIX_KEY_BRIGHTNESSDOWN) (void)backlight_step(0);
}

static void drain(void) {
    for (unsigned count = 0; count < EC_DRAIN_MAX; count++) {
        if (!(inb(EC_COMMAND_PORT) & EC_SCI_EVENT)) return;
        uint8_t query = 0;
        if (ec_query(&query) != 0 || !query) return;
        handle_query(query);
    }
}

static void ec_thread(void *unused) {
    (void)unused;
    for (;;) {
        drain();
        process_prepare_wait(&ec_channel, time_uptime_ns() + EC_POLL_NS);
        process_wait();
        process_finish_wait();
    }
}

static int field_is(const uint8_t *field, unsigned bytes, const char *text) {
    size_t length = strlen(text);
    if (length > bytes || memcmp(field, text, length) != 0) return 0;
    for (size_t index = length; index < bytes; index++)
        if (field[index] != 0 && field[index] != ' ') return 0;
    return 1;
}

static const struct ec_board *find_board(char *oem, char *table) {
    char signature[4];
    uint32_t length = 0;
    const uint8_t *dsdt = NULL;
    for (unsigned index = 0;; index++) {
        const void *found = acpi_table_at(index, signature, &length);
        if (!found) break;
        if (memcmp(signature, "DSDT", 4) == 0) {
            dsdt = found;
            break;
        }
    }
    if (!dsdt || length < DSDT_TABLE_ID + DSDT_TABLE_BYTES) return NULL;
    memcpy(oem, dsdt + DSDT_OEM_ID, DSDT_OEM_ID_BYTES);
    oem[DSDT_OEM_ID_BYTES] = '\0';
    memcpy(table, dsdt + DSDT_TABLE_ID, DSDT_TABLE_BYTES);
    table[DSDT_TABLE_BYTES] = '\0';
    for (unsigned index = 0; index < sizeof(boards) / sizeof(boards[0]); index++)
        if (field_is(dsdt + DSDT_OEM_ID, DSDT_OEM_ID_BYTES, boards[index].oem) &&
            field_is(dsdt + DSDT_TABLE_ID, DSDT_TABLE_BYTES, boards[index].table))
            return &boards[index];
    return NULL;
}

void ec_init(void) {
    const struct acpi_power *power = acpi_power_info();
    if (!power || !power->embedded_controller) return;
    const char *choice = boot_command_line_value("ec");
    if (choice && strncmp(choice, "off", 3) == 0) return;
    if (inb(EC_COMMAND_PORT) == 0xFFU) return;

    char oem[DSDT_OEM_ID_BYTES + 1], table[DSDT_TABLE_BYTES + 1];
    board = find_board(oem, table);
    if (!board) {
        kprintf("EC: no hotkey map for %s %s, left alone\n", oem, table);
        return;
    }
    if (!process_create_kthread("kec", ec_thread, NULL)) {
        kprintf("EC: cannot start the event thread\n");
        return;
    }
    kprintf("EC: hotkeys for %s %s\n", board->oem, board->table);
}

#else

void ec_init(void) {}

#endif
