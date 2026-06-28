#include <stdint.h>
#include "include/input.h"
#include "include/io.h"
#include "include/tty.h"

#define PS2_STATUS_PORT  0x64U
#define PS2_COMMAND_PORT 0x64U
#define PS2_DATA_PORT    0x60U

#define PS2_STATUS_OUTPUT_FULL 0x01U
#define PS2_STATUS_INPUT_FULL  0x02U
#define PS2_STATUS_AUX_DATA    0x20U
#define PS2_TIMEOUT 200000U
#define RAW_INPUT_CAPACITY 256U

static uint8_t raw_input[RAW_INPUT_CAPACITY];
static size_t raw_head;
static size_t raw_tail;
static size_t raw_count;
static unsigned raw_listeners;

static int ps2_wait_write(void) {
    for (unsigned i = 0; i < PS2_TIMEOUT; i++) {
        if (!(inb(PS2_STATUS_PORT) & PS2_STATUS_INPUT_FULL)) return 0;
        __asm__ volatile("pause");
    }
    return -1;
}

static int ps2_wait_read(uint8_t *status_out) {
    for (unsigned i = 0; i < PS2_TIMEOUT; i++) {
        uint8_t status = inb(PS2_STATUS_PORT);
        if (status & PS2_STATUS_OUTPUT_FULL) {
            if (status_out) *status_out = status;
            return 0;
        }
        __asm__ volatile("pause");
    }
    return -1;
}

static int ps2_command(uint8_t command) {
    if (ps2_wait_write() != 0) return -1;
    outb(PS2_COMMAND_PORT, command);
    return 0;
}

static int ps2_write_data(uint8_t value) {
    if (ps2_wait_write() != 0) return -1;
    outb(PS2_DATA_PORT, value);
    return 0;
}

static void raw_push(uint8_t value) {
    if (!raw_listeners) return;
    if (raw_count == RAW_INPUT_CAPACITY) {
        raw_head = (raw_head + 1U) % RAW_INPUT_CAPACITY;
        raw_count--;
    }
    raw_input[raw_tail] = value;
    raw_tail = (raw_tail + 1U) % RAW_INPUT_CAPACITY;
    raw_count++;
}

void input_init(void) {
    /* Configure the first PS/2 port for IRQ1 and translated set-1 scancodes.
     * Initialization runs with CPU interrupts disabled, so controller replies
     * can be consumed synchronously without racing the IRQ handler. */
    (void)ps2_command(0xADU); /* Disable first port while changing config. */
    (void)ps2_command(0xA7U); /* Keep the mouse/second port disabled. */
    while (inb(PS2_STATUS_PORT) & PS2_STATUS_OUTPUT_FULL)
        (void)inb(PS2_DATA_PORT);

    uint8_t config_status = 0;
    uint8_t config = 0;
    if (ps2_command(0x20U) == 0 && ps2_wait_read(&config_status) == 0 &&
        !(config_status & PS2_STATUS_AUX_DATA)) {
        config = inb(PS2_DATA_PORT);
        config |= 0x01U;  /* Enable keyboard IRQ1. */
        config &= (uint8_t)~0x02U; /* Disable mouse IRQ12. */
        config |= 0x40U;  /* Translate keyboard set 2 to set 1. */
        if (ps2_command(0x60U) == 0) (void)ps2_write_data(config);
    }
    (void)ps2_command(0xAEU); /* Enable first port. */

    /* Ensure keyboard scanning is enabled and consume the ACK before IRQ1 is
     * unmasked. A missing ACK is harmless; polling remains available. */
    if (ps2_write_data(0xF4U) == 0 && ps2_wait_read(&config_status) == 0)
        (void)inb(PS2_DATA_PORT);

    raw_head = 0;
    raw_tail = 0;
    raw_count = 0;
    raw_listeners = 0;
    tty_reset_keyboard_state();
}

static void input_drain_controller(void) {
    for (;;) {
        uint8_t status = inb(PS2_STATUS_PORT);
        if (!(status & PS2_STATUS_OUTPUT_FULL)) return;
        uint8_t value = inb(PS2_DATA_PORT);
        if (!(status & PS2_STATUS_AUX_DATA)) {
            raw_push(value);
            tty_handle_scancode(value);
        }
    }
}

void input_poll(void) {
    /* Kept as a fallback for early boot and hardware that does not deliver
     * legacy IRQ1. Normal userspace input is captured by input_irq(). */
    input_drain_controller();
}

void input_irq(void) {
    input_drain_controller();
}

void input_scancode_open(void) {
    raw_listeners++;
}

void input_scancode_close(void) {
    if (raw_listeners) raw_listeners--;
    if (!raw_listeners) {
        raw_head = 0;
        raw_tail = 0;
        raw_count = 0;
    }
}

int input_scancodes_ready(void) {
    input_poll();
    return raw_count != 0;
}

int64_t input_read_scancodes(size_t size, void *buffer) {
    if (!buffer) return -1;
    if (!size) return 0;
    while (!raw_count) {
        input_poll();
        __asm__ volatile("pause");
    }
    uint8_t *out = (uint8_t *)buffer;
    size_t completed = 0;
    while (completed < size && raw_count) {
        out[completed++] = raw_input[raw_head];
        raw_head = (raw_head + 1U) % RAW_INPUT_CAPACITY;
        raw_count--;
    }
    return (int64_t)completed;
}
