#include <stddef.h>
#include <stdint.h>
#include "../include/heap.h"
extern void kprintf(const char *fmt, ...);
#include "../include/boot.h"
#include "../include/input.h"
#include "../include/io.h"
#include "../include/kstring.h"
#include "../include/process.h"
#include "../include/time.h"
#include "../include/xhci.h"
#include "../include/tty.h"
#include "../include/usercopy.h"
#include "../include/vt.h"
#include "../include/tunix/input_event.h"

#define EAGAIN 11
#define EINVAL 22
#define EFAULT 14
#define ENOENT 2
#define ENOTTY 25

#define input_copy_to_user copy_to_user
#define input_copy_from_user copy_from_user

#define PS2_STATUS_PORT  0x64U
#define PS2_COMMAND_PORT 0x64U
#define PS2_DATA_PORT    0x60U

#define PS2_STATUS_OUTPUT_FULL 0x01U
#define PS2_STATUS_INPUT_FULL  0x02U
#define PS2_STATUS_AUX_DATA    0x20U
#define PS2_TIMEOUT 200000U

#define PS2_ACK 0xFAU
#define PS2_MOUSE_WRITE 0xD4U

#define RAW_INPUT_CAPACITY 256U
#define INPUT_READER_CAPACITY 128U
#define INPUT_KEY_STATE_SIZE 256U

#define EVDEV_CLOCK_REALTIME 0
#define EVDEV_CLOCK_MONOTONIC 1

/*
 * The queue holds a plain nanosecond timestamp rather than the timeval the
 * device reports, because the clock is a property of the *reader*: EVIOCSCLOCKID
 * lets each descriptor pick realtime or monotonic, and libinput switches to
 * monotonic the moment it opens the device. Converting at read time is what
 * makes that possible without keeping two copies of every event.
 */
struct input_record {
    uint64_t time_ns;
    uint16_t type;
    uint16_t code;
    int32_t value;
};

struct input_reader {
    unsigned device_id;
    /*
     * The virtual terminal this descriptor belongs to: the one its opener was
     * on. Events are only delivered while that terminal is the active one,
     * which is what stops an X server on tty7 from reading along with the
     * password being typed at a login on tty2. It is the job logind does on a
     * Linux desktop, and there is nothing here to do it anywhere else.
     */
    unsigned vt_index;
    struct input_record events[INPUT_READER_CAPACITY];
    size_t head;
    size_t tail;
    size_t count;
    /* CLOCK_REALTIME (0) or CLOCK_MONOTONIC (1), chosen with EVIOCSCLOCKID. */
    int clock_id;
    /* EVIOCGRAB is exclusive access. Nothing here multiplexes a device between
       compositors, so the grab is recorded and honoured but never contested. */
    int grabbed;
    struct input_reader *next;
};

static uint8_t raw_input[RAW_INPUT_CAPACITY];
static size_t raw_head;
static size_t raw_tail;
static size_t raw_count;
static unsigned raw_listeners;

static struct input_reader *input_readers;

/* Whether to say what each key event was, asked for with `inputlog`. */
/* Bounded, because a stuck key would otherwise fill the console. */
#define INPUT_LOG_LIMIT 120U
static int input_logging;
static uint8_t key_down[INPUT_KEY_STATE_SIZE];
static unsigned keyboard_extended;
static unsigned keyboard_pause_bytes;

/* Defined with the rest of the reader bookkeeping, further down; the keyboard
   path needs it to know whether the console still owns the keystrokes. */
static int device_has_reader(unsigned device_id);

/* Whether there is an i8042 at all. Read once, at init: an absent controller
   answers every port with 0xFF, and taking that for real status is an endless
   loop rather than a missing keyboard. */
static unsigned ps2_present;

static uint8_t mouse_packet[4];
static unsigned mouse_packet_index;
static unsigned mouse_packet_size;
static unsigned mouse_device_id;
static unsigned mouse_present;
static uint8_t mouse_buttons;

static uint64_t interrupt_save(void) {
    uint64_t flags;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) : : "memory");
    return flags;
}

static void interrupt_restore(uint64_t flags) {
    __asm__ volatile("pushq %0; popfq" : : "r"(flags) : "memory", "cc");
}

static int ps2_wait_write(void) {
    for (unsigned i = 0; i < PS2_TIMEOUT; i++) {
        if (!(inb(PS2_STATUS_PORT) & PS2_STATUS_INPUT_FULL)) return 0;
        __asm__ volatile("pause");
    }
    return -1;
}

static int ps2_wait_read(int expect_aux, uint8_t *value) {
    for (unsigned i = 0; i < PS2_TIMEOUT; i++) {
        uint8_t status = inb(PS2_STATUS_PORT);
        if (status & PS2_STATUS_OUTPUT_FULL) {
            uint8_t data = inb(PS2_DATA_PORT);
            int is_aux = (status & PS2_STATUS_AUX_DATA) != 0;
            if (expect_aux < 0 || is_aux == expect_aux) {
                if (value) *value = data;
                return 0;
            }
            continue;
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

static int ps2_read_config(uint8_t *config) {
    if (!config || ps2_command(0x20U) != 0) return -1;
    return ps2_wait_read(0, config);
}

static int ps2_write_config(uint8_t config) {
    if (ps2_command(0x60U) != 0) return -1;
    return ps2_write_data(config);
}

static void ps2_flush(void) {
    unsigned remaining = 64U;
    while (remaining-- && (inb(PS2_STATUS_PORT) & PS2_STATUS_OUTPUT_FULL))
        (void)inb(PS2_DATA_PORT);
}

static int ps2_keyboard_command(uint8_t command) {
    uint8_t response;
    if (ps2_write_data(command) != 0 || ps2_wait_read(0, &response) != 0)
        return -1;
    return response == PS2_ACK ? 0 : -1;
}

static int ps2_mouse_command(uint8_t command) {
    uint8_t response;
    if (ps2_command(PS2_MOUSE_WRITE) != 0 || ps2_write_data(command) != 0 ||
        ps2_wait_read(1, &response) != 0)
        return -1;
    return response == PS2_ACK ? 0 : -1;
}

static int ps2_mouse_set_sample_rate(uint8_t rate) {
    if (ps2_mouse_command(0xF3U) != 0) return -1;
    return ps2_mouse_command(rate);
}

static int ps2_mouse_get_id(uint8_t *device_id) {
    if (!device_id || ps2_mouse_command(0xF2U) != 0) return -1;
    return ps2_wait_read(1, device_id);
}

static unsigned ps2_mouse_negotiate_wheel(void) {
    uint8_t device_id = 0;
    if (ps2_mouse_set_sample_rate(200U) != 0 ||
        ps2_mouse_set_sample_rate(100U) != 0 ||
        ps2_mouse_set_sample_rate(80U) != 0 ||
        ps2_mouse_get_id(&device_id) != 0)
        return 0U;

    if (device_id == 3U) {
        uint8_t explorer_id = device_id;
        if (ps2_mouse_set_sample_rate(200U) == 0 &&
            ps2_mouse_set_sample_rate(200U) == 0 &&
            ps2_mouse_set_sample_rate(80U) == 0 &&
            ps2_mouse_get_id(&explorer_id) == 0 && explorer_id == 4U)
            return 4U;
        return 3U;
    }
    return device_id == 4U ? 4U : 0U;
}

static void reader_push(struct input_reader *reader,
                        const struct input_record *event) {
    if (!reader || !event) return;
    if (reader->count == INPUT_READER_CAPACITY) {
        reader->head = 0;
        reader->tail = 0;
        reader->count = 0;
        struct input_record dropped = {
            .time_ns = event->time_ns,
            .type = TUNIX_EV_SYN,
            .code = TUNIX_SYN_DROPPED,
            .value = 0
        };
        reader->events[reader->tail] = dropped;
        reader->tail = (reader->tail + 1U) % INPUT_READER_CAPACITY;
        reader->count = 1U;
    }
    reader->events[reader->tail] = *event;
    reader->tail = (reader->tail + 1U) % INPUT_READER_CAPACITY;
    reader->count++;
}

static struct input_key_event key_history[INPUT_KEY_HISTORY];
/* Every event ever, so the ring can be handed back oldest first. */
static unsigned key_history_total;

unsigned input_key_history_count(void) {
    return key_history_total;
}

int input_key_history_at(unsigned index, struct input_key_event *out) {
    if (!out || index >= key_history_total) return -1;
    if (key_history_total > INPUT_KEY_HISTORY &&
        index < key_history_total - INPUT_KEY_HISTORY) return -1;
    uint64_t flags = interrupt_save();
    *out = key_history[index % INPUT_KEY_HISTORY];
    interrupt_restore(flags);
    return 0;
}

static void remember_key(uint64_t timestamp, uint16_t code, int32_t value,
                         unsigned delivered) {
    struct input_key_event *slot = &key_history[key_history_total % INPUT_KEY_HISTORY];
    slot->millisecond = (uint32_t)(timestamp / 1000000ULL);
    slot->code = code;
    slot->value = (uint16_t)value;
    slot->readers = (uint16_t)delivered;
    key_history_total++;
}

static void input_emit_at(unsigned device_id, uint64_t timestamp,
                          uint16_t type, uint16_t code, int32_t value) {
    struct input_record event = {
        .time_ns = timestamp,
        .type = type,
        .code = code,
        .value = value
    };
    /* What the kernel actually produced, when `inputlog` asks. */
    /* A key that arrives once and is typed several times is either delivered
       twice here or delivered once and repeated above us, and only the two
       timestamps of a press and its release tell those apart. */
    unsigned delivered = 0;
    for (struct input_reader *reader = input_readers; reader; reader = reader->next) {
        if (reader->device_id != device_id) continue;
        if (!vt_input_delivered_to(reader->vt_index)) continue;
        reader_push(reader, &event);
        delivered++;
    }
    if (type == TUNIX_EV_KEY) remember_key(timestamp, code, value, delivered);
    if (input_logging && type == TUNIX_EV_KEY) {
        static unsigned logged;
        if (logged < INPUT_LOG_LIMIT) {
            logged++;
            kprintf("INPUT: key code %u value %d readers %u at %u ms\n",
                    (unsigned)code, (int)value, delivered,
                    (unsigned)(timestamp / 1000000ULL));
        }
    }
}

static void input_sync_at(unsigned device_id, uint64_t timestamp) {
    input_emit_at(device_id, timestamp, TUNIX_EV_SYN, TUNIX_SYN_REPORT, 0);
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

static uint16_t extended_keycode(uint8_t scan) {
    switch (scan) {
        case 0x1CU: return TUNIX_KEY_KPENTER;
        case 0x1DU: return TUNIX_KEY_RIGHTCTRL;
        case 0x35U: return TUNIX_KEY_KPSLASH;
        case 0x37U: return TUNIX_KEY_SYSRQ;
        case 0x38U: return TUNIX_KEY_RIGHTALT;
        case 0x47U: return TUNIX_KEY_HOME;
        case 0x48U: return TUNIX_KEY_UP;
        case 0x49U: return TUNIX_KEY_PAGEUP;
        case 0x4BU: return TUNIX_KEY_LEFT;
        case 0x4DU: return TUNIX_KEY_RIGHT;
        case 0x4FU: return TUNIX_KEY_END;
        case 0x50U: return TUNIX_KEY_DOWN;
        case 0x51U: return TUNIX_KEY_PAGEDOWN;
        case 0x52U: return TUNIX_KEY_INSERT;
        case 0x53U: return TUNIX_KEY_DELETE;
        case 0x5BU: return TUNIX_KEY_LEFTMETA;
        case 0x5CU: return TUNIX_KEY_RIGHTMETA;
        case 0x5DU: return TUNIX_KEY_COMPOSE;
        default: return TUNIX_KEY_RESERVED;
    }
}

/*
 * One key, on its way to everything that cares.
 *
 * Returns 0 when the VT layer took the key for itself, which is how
 * Ctrl+Alt+F2 reaches the kernel and nothing else: not the evdev readers, so a
 * compositor holding a grab cannot keep the user from leaving it, and not the
 * console either, so no shell ever sees the F-key that moved the screen.
 */
static int keyboard_emit_key(uint16_t keycode, int released) {
    if (!keycode || keycode >= INPUT_KEY_STATE_SIZE) return 1;
    int ctrl_held = key_down[TUNIX_KEY_LEFTCTRL] || key_down[TUNIX_KEY_RIGHTCTRL];
    int alt_held = key_down[TUNIX_KEY_LEFTALT] || key_down[TUNIX_KEY_RIGHTALT];
    if (vt_handle_hotkey(keycode, !released, ctrl_held, alt_held)) {
        /* Still recorded as held or not: the key is real even though nobody
           downstream is told about it, and the record is what stops a later
           release from being read as a press. */
        key_down[keycode] = released ? 0 : 1;
        return 0;
    }
    int32_t value;
    if (released) {
        value = 0;
        key_down[keycode] = 0;
    } else if (key_down[keycode]) {
        value = 2;
    } else {
        value = 1;
        key_down[keycode] = 1;
    }
    uint64_t timestamp = time_uptime_ns();
    input_emit_at(TUNIX_INPUT_DEVICE_KEYBOARD, timestamp,
                  TUNIX_EV_KEY, keycode, value);
    input_sync_at(TUNIX_INPUT_DEVICE_KEYBOARD, timestamp);

    /*
     * And the console, from the same place -- which is what makes a USB
     * keyboard able to type at a login prompt. The PS/2 path used to hand its
     * raw scancodes to the console separately, so a machine with no PS/2 port
     * could switch terminals but not type at one.
     *
     * When an input stack (Xorg/libinput, a Wayland compositor) has the
     * keyboard open on the terminal in front, it owns the keystrokes and the
     * console must not also cook them: a keystroke typed into a window would
     * otherwise also hit the shell underneath, and Ctrl+C would fire a console
     * SIGINT that tears the whole session down.
     */
    if (!device_has_reader(TUNIX_INPUT_DEVICE_KEYBOARD))
        vt_handle_key(keycode, released ? 0 : 1);
    return 1;
}

/* Defined with the PS/2 packet handling, which wants the same helper. */
static void mouse_emit_button(uint64_t timestamp, uint8_t changed,
                              uint8_t state, uint8_t bit, uint16_t code);

/*
 * The way in for a keyboard that is not on the PS/2 port.
 *
 * USB HID reports carry usages, not scancodes, and the driver that reads them
 * has already turned those into keycodes -- so they join the path here, past
 * the translation, and everything downstream cannot tell the two keyboards
 * apart. A machine with both ends up with one keyboard device fed from two
 * places, which is what a user expects of it.
 */
void input_external_key(uint16_t keycode, int released) {
    (void)keyboard_emit_key(keycode, released);
}

/* The same door for a pointer. Button state is a bitmap in the same order the
   PS/2 packet uses, so the two share the change-detection below. */
void input_external_mouse(int dx, int dy, int wheel, uint8_t buttons) {
    uint8_t changed = buttons ^ mouse_buttons;
    if (!dx && !dy && !wheel && !changed) return;

    uint64_t timestamp = time_uptime_ns();
    if (dx) input_emit_at(TUNIX_INPUT_DEVICE_MOUSE, timestamp, TUNIX_EV_REL,
                          TUNIX_REL_X, dx);
    /* HID counts Y downwards, which is the direction the rest of the system
       wants; the PS/2 path negates because that one counts upwards. */
    if (dy) input_emit_at(TUNIX_INPUT_DEVICE_MOUSE, timestamp, TUNIX_EV_REL,
                          TUNIX_REL_Y, dy);
    if (wheel) input_emit_at(TUNIX_INPUT_DEVICE_MOUSE, timestamp, TUNIX_EV_REL,
                             TUNIX_REL_WHEEL, wheel);
    mouse_emit_button(timestamp, changed, buttons, 0x01U, TUNIX_BTN_LEFT);
    mouse_emit_button(timestamp, changed, buttons, 0x02U, TUNIX_BTN_RIGHT);
    mouse_emit_button(timestamp, changed, buttons, 0x04U, TUNIX_BTN_MIDDLE);
    mouse_buttons = buttons;
    input_sync_at(TUNIX_INPUT_DEVICE_MOUSE, timestamp);
}

static int keyboard_handle_event_byte(uint8_t byte) {
    if (keyboard_pause_bytes) {
        keyboard_pause_bytes--;
        return 0;
    }
    if (byte == 0xE1U) {
        keyboard_pause_bytes = 5U;
        uint64_t timestamp = time_uptime_ns();
        input_emit_at(TUNIX_INPUT_DEVICE_KEYBOARD, timestamp,
                      TUNIX_EV_KEY, TUNIX_KEY_PAUSE, 1);
        input_sync_at(TUNIX_INPUT_DEVICE_KEYBOARD, timestamp);
        input_emit_at(TUNIX_INPUT_DEVICE_KEYBOARD, timestamp,
                      TUNIX_EV_KEY, TUNIX_KEY_PAUSE, 0);
        input_sync_at(TUNIX_INPUT_DEVICE_KEYBOARD, timestamp);
        return 1;
    }
    if (byte == 0xE0U) {
        keyboard_extended = 1U;
        return 1;
    }

    int released = (byte & 0x80U) != 0;
    uint8_t scan = byte & 0x7FU;
    uint16_t keycode;
    if (keyboard_extended) {
        keyboard_extended = 0;
        /* PrintScreen emits fake extended shifts; they are not real keys. */
        if (scan == 0x2AU || scan == 0x36U) return 1;
        keycode = extended_keycode(scan);
    } else {
        keycode = scan <= TUNIX_KEY_F12 ? scan : TUNIX_KEY_RESERVED;
    }
    /* When the VT layer takes the key, the console must not be given the
       scancode either -- otherwise the F-key that switched the screen also
       arrives at the shell that was left behind. */
    return keyboard_emit_key(keycode, released);
}

static void mouse_emit_button(uint64_t timestamp, uint8_t changed,
                              uint8_t state, uint8_t bit, uint16_t code) {
    if (!(changed & bit)) return;
    input_emit_at(TUNIX_INPUT_DEVICE_MOUSE, timestamp, TUNIX_EV_KEY, code,
                  (state & bit) ? 1 : 0);
}

static void mouse_complete_packet(void) {
    uint8_t flags = mouse_packet[0];
    uint8_t new_buttons = flags & 0x07U;
    int x = (int)mouse_packet[1] - ((flags & 0x10U) ? 256 : 0);
    int y = (int)mouse_packet[2] - ((flags & 0x20U) ? 256 : 0);
    int wheel = 0;

    if (mouse_packet_size == 4U) {
        uint8_t fourth = mouse_packet[3];
        if (mouse_device_id == 4U) {
            /* The Explorer spends the top of the byte on its two extra
               buttons, so only the low nibble is the wheel. */
            wheel = (int)(fourth & 0x0FU);
            if (wheel & 0x08) wheel -= 16;
            if (fourth & 0x10U) new_buttons |= 0x08U;
            if (fourth & 0x20U) new_buttons |= 0x10U;
        } else {
            wheel = (int)(int8_t)fourth;
        }
    }

    if (flags & 0xC0U) {
        x = 0;
        y = 0;
    }

    uint8_t changed = new_buttons ^ mouse_buttons;
    if (!x && !y && !wheel && !changed) return;

    uint64_t timestamp = time_uptime_ns();
    if (x) input_emit_at(TUNIX_INPUT_DEVICE_MOUSE, timestamp, TUNIX_EV_REL,
                         TUNIX_REL_X, x);
    if (y) input_emit_at(TUNIX_INPUT_DEVICE_MOUSE, timestamp, TUNIX_EV_REL,
                         TUNIX_REL_Y, -y);
    /* The wheel is negated for the same reason Y is: the mouse counts a push
       away from the hand as negative, and REL_WHEEL counts it as positive.
       Without this every scroll goes the wrong way. */
    if (wheel) input_emit_at(TUNIX_INPUT_DEVICE_MOUSE, timestamp, TUNIX_EV_REL,
                             TUNIX_REL_WHEEL, -wheel);
    mouse_emit_button(timestamp, changed, new_buttons, 0x01U, TUNIX_BTN_LEFT);
    mouse_emit_button(timestamp, changed, new_buttons, 0x02U, TUNIX_BTN_RIGHT);
    mouse_emit_button(timestamp, changed, new_buttons, 0x04U, TUNIX_BTN_MIDDLE);
    mouse_emit_button(timestamp, changed, new_buttons, 0x08U, TUNIX_BTN_SIDE);
    mouse_emit_button(timestamp, changed, new_buttons, 0x10U, TUNIX_BTN_EXTRA);
    mouse_buttons = new_buttons;
    input_sync_at(TUNIX_INPUT_DEVICE_MOUSE, timestamp);
}

static void mouse_handle_byte(uint8_t byte) {
    if (!mouse_present) return;
    if (mouse_packet_index == 0U && !(byte & 0x08U)) return;
    mouse_packet[mouse_packet_index++] = byte;
    if (mouse_packet_index == mouse_packet_size) {
        mouse_packet_index = 0;
        mouse_complete_packet();
    }
}

void input_init(void) {
    input_logging = boot_command_line_flag("inputlog");
    uint8_t config = 0;

    raw_head = 0;
    raw_tail = 0;
    raw_count = 0;
    raw_listeners = 0;
    input_readers = NULL;
    memset(key_down, 0, sizeof(key_down));
    keyboard_extended = 0;
    keyboard_pause_bytes = 0;
    mouse_packet_index = 0;
    mouse_buttons = 0;
    mouse_present = 0;
    tty_reset_keyboard_state();

    /* Is the controller there at all? A machine whose keyboard is on USB may
       have no i8042, and every port of one that is missing reads back 0xFF.
       Everything below talks to it, so this question comes first. */
    ps2_present = inb(PS2_STATUS_PORT) != 0xFFU;
    if (!ps2_present) return;

    (void)ps2_command(0xADU);
    (void)ps2_command(0xA7U);
    ps2_flush();

    if (ps2_read_config(&config) != 0) config = 0;
    config &= (uint8_t)~0x03U;
    config |= 0x40U;
    (void)ps2_write_config(config);

    (void)ps2_command(0xAEU);
    (void)ps2_command(0xA8U);

    config |= 0x03U;
    config &= (uint8_t)~0x30U;
    config |= 0x40U;
    (void)ps2_write_config(config);

    (void)ps2_keyboard_command(0xF4U);

    mouse_present = ps2_mouse_command(0xF6U) == 0;
    mouse_device_id = 0U;
    mouse_packet_size = 3U;
    if (mouse_present) {
        mouse_device_id = ps2_mouse_negotiate_wheel();
        if (mouse_device_id == 3U || mouse_device_id == 4U)
            mouse_packet_size = 4U;
        if (ps2_mouse_command(0xF4U) != 0) mouse_present = 0;
    }

}

/* Is any descriptor reading this device? A userspace input stack that has the
   keyboard open owns it -- Xorg's libinput driver (VT-less, so it never
   EVIOCGRABs) as much as a Wayland compositor (which does grab). Either way the
   kernel console must stop cooking the same keystrokes, or Ctrl+C typed into a
   terminal under X fires a console SIGINT at the whole session. So gate on an
   open reader, not just an exclusive grab. */
static int device_has_reader(unsigned device_id) {
    for (struct input_reader *reader = input_readers; reader; reader = reader->next)
        if (reader->device_id == device_id && vt_input_delivered_to(reader->vt_index))
            return 1;
    return 0;
}

static void input_drain_controller(void) {
    if (!ps2_present) return;
    for (;;) {
        uint8_t status = inb(PS2_STATUS_PORT);
        /* All ones is an absent controller, not a full output buffer. A
           machine with no i8042 -- which is every machine whose keyboard is on
           USB, and QEMU with i8042=off -- would otherwise read 0xFF for ever
           and never leave this loop. */
        if (status == 0xFFU) return;
        if (!(status & PS2_STATUS_OUTPUT_FULL)) return;
        uint8_t value = inb(PS2_DATA_PORT);
        if (status & PS2_STATUS_AUX_DATA) {
            mouse_handle_byte(value);
        } else {
            /* The raw byte is kept for /dev/input/keyboard, the one reader
               that still wants scancodes. Everything else -- evdev and the
               console alike -- is fed the keycode this decodes to. */
            raw_push(value);
            (void)keyboard_handle_event_byte(value);
        }
    }
}

void input_poll(void) {
    uint64_t flags = interrupt_save();
    input_drain_controller();
    interrupt_restore(flags);
    /* Outside the interrupt-off window: the USB poll talks to a controller
       over MMIO and waits on it, which is not work to do with interrupts
       masked. It has no shared state with the PS/2 path above. */
    xhci_poll();
}

void input_irq(void) {
    input_drain_controller();
}

/* A pointer of either kind. The PS/2 one is what this driver negotiated; the
   USB one belongs to the HID driver, and both feed the same evdev device. */
int input_mouse_available(void) {
    return mouse_present != 0 || xhci_pointer_present();
}

int input_get_device_info(unsigned device_id, struct tunix_input_device_info *info) {
    if (!info) return -EINVAL;
    memset(info, 0, sizeof(*info));
    info->abi_version = TUNIX_INPUT_ABI_VERSION;
    info->device_id = device_id;
    if (device_id == TUNIX_INPUT_DEVICE_KEYBOARD) {
        info->event_types = (1U << TUNIX_EV_SYN) | (1U << TUNIX_EV_KEY);
        info->capabilities = TUNIX_INPUT_CAP_KEYBOARD;
        memcpy(info->name, "Tunix PS/2 Keyboard", sizeof("Tunix PS/2 Keyboard"));
        return 0;
    }
    if (device_id == TUNIX_INPUT_DEVICE_MOUSE && mouse_present) {
        info->event_types = (1U << TUNIX_EV_SYN) | (1U << TUNIX_EV_KEY) |
                            (1U << TUNIX_EV_REL);
        info->relative_axes = (1U << TUNIX_REL_X) | (1U << TUNIX_REL_Y);
        info->capabilities = TUNIX_INPUT_CAP_POINTER;
        if (mouse_packet_size == 4U) {
            info->relative_axes |= 1U << TUNIX_REL_WHEEL;
            info->capabilities |= TUNIX_INPUT_CAP_WHEEL;
        }
        if (mouse_device_id == 4U)
            info->capabilities |= TUNIX_INPUT_CAP_EXTRA_BUTTONS;
        memcpy(info->name, "Tunix PS/2 Mouse", sizeof("Tunix PS/2 Mouse"));
        return 0;
    }
    return -EINVAL;
}

void input_scancode_open(void) {
    uint64_t flags = interrupt_save();
    raw_listeners++;
    interrupt_restore(flags);
}

void input_scancode_close(void) {
    uint64_t flags = interrupt_save();
    if (raw_listeners) raw_listeners--;
    if (!raw_listeners) {
        raw_head = 0;
        raw_tail = 0;
        raw_count = 0;
    }
    interrupt_restore(flags);
}

int input_scancodes_ready(void) {
    input_poll();
    uint64_t flags = interrupt_save();
    int ready = raw_count != 0;
    interrupt_restore(flags);
    return ready;
}

int64_t input_read_scancodes(size_t size, void *buffer) {
    if (!buffer) return -EINVAL;
    if (!size) return 0;
    input_poll();
    uint64_t flags = interrupt_save();
    if (!raw_count) {
        interrupt_restore(flags);
        return -EAGAIN;
    }
    uint8_t *out = (uint8_t *)buffer;
    size_t completed = 0;
    while (completed < size && raw_count) {
        out[completed++] = raw_input[raw_head];
        raw_head = (raw_head + 1U) % RAW_INPUT_CAPACITY;
        raw_count--;
    }
    interrupt_restore(flags);
    return (int64_t)completed;
}

struct input_reader *input_reader_open(unsigned device_id) {
    if (device_id != TUNIX_INPUT_DEVICE_KEYBOARD &&
        device_id != TUNIX_INPUT_DEVICE_MOUSE)
        return NULL;
    if (device_id == TUNIX_INPUT_DEVICE_MOUSE && !mouse_present) return NULL;

    struct input_reader *reader = (struct input_reader *)kmalloc(sizeof(*reader));
    if (!reader) return NULL;
    memset(reader, 0, sizeof(*reader));
    reader->device_id = device_id;
    reader->vt_index = vt_current_index();

    uint64_t flags = interrupt_save();
    reader->next = input_readers;
    input_readers = reader;
    unsigned total = 0;
    for (struct input_reader *walk = input_readers; walk; walk = walk->next)
        if (walk->device_id == device_id) total++;
    interrupt_restore(flags);
    /* One line per open, so a device that ends up with more readers than there
       are openers says so. */
    if (input_logging)
        kprintf("INPUT: open device %u vt %u by pid %d, %u readers now\n",
                device_id, reader->vt_index, (int)process_current_pid(), total);
    return reader;
}

void input_reader_close(struct input_reader *reader) {
    if (!reader) return;
    uint64_t flags = interrupt_save();
    struct input_reader **link = &input_readers;
    while (*link && *link != reader) link = &(*link)->next;
    if (*link == reader) *link = reader->next;
    int was_grabbed = reader->grabbed;
    interrupt_restore(flags);
    /* A compositor that exited or crashed while holding a grab hands the
       keyboard back here; the console has been deaf since the grab. */
    if (was_grabbed) tty_reset_keyboard_state();
    if (input_logging)
        kprintf("INPUT: close device %u vt %u\n", reader->device_id,
                reader->vt_index);
    kfree(reader);
}

int input_reader_ready(struct input_reader *reader) {
    if (!reader) return 0;
    input_poll();
    uint64_t flags = interrupt_save();
    int ready = reader->count != 0;
    interrupt_restore(flags);
    return ready;
}

/* --- evdev ioctls -------------------------------------------------------- */

/*
 * Linux's evdev interface, which is what libinput speaks. Like the DRM ioctls
 * these are matched by decoding the request rather than against fully encoded
 * constants: EVIOCGNAME, EVIOCGBIT and friends carry their buffer length and
 * their axis in the request itself, so there is no single constant to compare
 * against.
 */
#define EVDEV_IOCTL_TYPE 'E'
#define EVDEV_IOCTL_TYPE_OF(request) (((request) >> 8) & 0xFFU)
#define EVDEV_IOCTL_NR(request) ((request) & 0xFFU)
#define EVDEV_IOCTL_SIZE(request) (((request) >> 16) & 0x3FFFU)

#define EVIOCGVERSION_NR 0x01
#define EVIOCGID_NR 0x02
#define EVIOCGNAME_NR 0x06
#define EVIOCGPHYS_NR 0x07
#define EVIOCGUNIQ_NR 0x08
#define EVIOCGPROP_NR 0x09
#define EVIOCGKEY_NR 0x18
#define EVIOCGLED_NR 0x19
#define EVIOCGSND_NR 0x1a
#define EVIOCGSW_NR 0x1b
#define EVIOCGBIT_NR 0x20   /* .. 0x3f, one per event type */
#define EVIOCGABS_NR 0x40   /* .. 0x7f, one per absolute axis */
#define EVIOCGRAB_NR 0x90
#define EVIOCREVOKE_NR 0x91
#define EVIOCSCLOCKID_NR 0xa0

/* The version every evdev driver in Linux reports. */
#define EVDEV_VERSION 0x010001

#define EV_MSC 0x04
#define EV_LED 0x11
#define EV_SW 0x05
#define EV_MAX 0x1f
#define KEY_MAX 0x2ff

struct evdev_id {
    uint16_t bustype;
    uint16_t vendor;
    uint16_t product;
    uint16_t version;
};

#define BUS_I8042 0x11

static void bitmap_set(uint8_t *bits, size_t limit, unsigned bit) {
    if (bit / 8U >= limit) return;
    bits[bit / 8U] |= (uint8_t)(1U << (bit % 8U));
}

/*
 * Which event types a device produces. Getting this wrong is not a degradation:
 * libinput classifies a device purely from its bits, so a keyboard that also
 * claimed EV_REL would be taken for a pointer.
 */
static void evdev_event_type_bits(unsigned device_id, uint8_t *bits, size_t limit) {
    memset(bits, 0, limit);
    bitmap_set(bits, limit, TUNIX_EV_SYN);
    bitmap_set(bits, limit, TUNIX_EV_KEY);
    if (device_id == TUNIX_INPUT_DEVICE_MOUSE) bitmap_set(bits, limit, TUNIX_EV_REL);
}

static void evdev_key_bits(unsigned device_id, uint8_t *bits, size_t limit) {
    memset(bits, 0, limit);
    if (device_id == TUNIX_INPUT_DEVICE_MOUSE) {
        bitmap_set(bits, limit, TUNIX_BTN_LEFT);
        bitmap_set(bits, limit, TUNIX_BTN_RIGHT);
        bitmap_set(bits, limit, TUNIX_BTN_MIDDLE);
        if (mouse_device_id == 4U) {
            bitmap_set(bits, limit, TUNIX_BTN_SIDE);
            bitmap_set(bits, limit, TUNIX_BTN_EXTRA);
        }
        return;
    }
    /* Every keycode the scancode tables can produce. They are contiguous from
       KEY_ESC to KEY_COMPOSE apart from gaps Linux leaves reserved, and
       claiming a reserved one costs nothing. */
    for (unsigned key = TUNIX_KEY_ESC; key <= TUNIX_KEY_COMPOSE; key++)
        bitmap_set(bits, limit, key);
}

static void evdev_rel_bits(unsigned device_id, uint8_t *bits, size_t limit) {
    memset(bits, 0, limit);
    if (device_id != TUNIX_INPUT_DEVICE_MOUSE) return;
    bitmap_set(bits, limit, TUNIX_REL_X);
    bitmap_set(bits, limit, TUNIX_REL_Y);
    if (mouse_packet_size == 4U) bitmap_set(bits, limit, TUNIX_REL_WHEEL);
}

/* Copy at most `size` bytes of a bitmap out, and report how many went. */
static int64_t evdev_copy_out(uint64_t user_argument, const void *source,
                              size_t available, size_t size) {
    if (!user_argument) return -EINVAL;
    size_t copy = size < available ? size : available;
    if (input_copy_to_user(user_argument, source, copy) != 0) return -EFAULT;
    return (int64_t)copy;
}

int64_t input_reader_ioctl(struct input_reader *reader, unsigned device_id,
                           unsigned long request, uint64_t user_argument) {
    if (EVDEV_IOCTL_TYPE_OF(request) != (unsigned)EVDEV_IOCTL_TYPE) return -ENOTTY;
    unsigned nr = EVDEV_IOCTL_NR(request);
    size_t size = EVDEV_IOCTL_SIZE(request);

    /* Big enough for KEY_MAX, which is the largest bitmap evdev defines. */
    uint8_t bits[(KEY_MAX / 8U) + 1U];

    if (nr == EVIOCGVERSION_NR) {
        int32_t version = EVDEV_VERSION;
        return evdev_copy_out(user_argument, &version, sizeof(version), size) < 0
                   ? -EFAULT : 0;
    }

    if (nr == EVIOCGID_NR) {
        /* An i8042 device with no meaningful vendor or product id -- which is
           the truth, and what a PS/2 controller reports on Linux too. */
        struct evdev_id id = { .bustype = BUS_I8042, .vendor = 0,
                               .product = 0, .version = 0 };
        return evdev_copy_out(user_argument, &id, sizeof(id), size) < 0
                   ? -EFAULT : 0;
    }

    if (nr == EVIOCGNAME_NR) {
        struct tunix_input_device_info info;
        if (input_get_device_info(device_id, &info) != 0) return -EINVAL;
        size_t length = 0;
        while (length < sizeof(info.name) && info.name[length]) length++;
        return evdev_copy_out(user_argument, info.name, length + 1U, size);
    }

    /* No physical topology and no serial number to report. ENOENT is what
       Linux answers for an absent string, and libevdev treats it as absent
       rather than as a failure. */
    if (nr == EVIOCGPHYS_NR || nr == EVIOCGUNIQ_NR) return -ENOENT;

    if (nr == EVIOCGPROP_NR) {
        /* INPUT_PROP_*: nothing here is a pointing stick, a buttonpad or a
           direct-touch device. */
        memset(bits, 0, sizeof(bits));
        return evdev_copy_out(user_argument, bits, size, size);
    }

    if (nr >= EVIOCGBIT_NR && nr <= EVIOCGBIT_NR + EV_MAX) {
        unsigned event_type = nr - EVIOCGBIT_NR;
        switch (event_type) {
        case 0: evdev_event_type_bits(device_id, bits, sizeof(bits)); break;
        case TUNIX_EV_KEY: evdev_key_bits(device_id, bits, sizeof(bits)); break;
        case TUNIX_EV_REL: evdev_rel_bits(device_id, bits, sizeof(bits)); break;
        /* Absolute axes, LEDs, switches, sound, force feedback: none. */
        default: memset(bits, 0, sizeof(bits)); break;
        }
        return evdev_copy_out(user_argument, bits, size, size);
    }

    /* Current state of the keys, LEDs, switches and sound. Only the key state
       is real; the rest are permanently clear. */
    if (nr == EVIOCGKEY_NR) {
        memset(bits, 0, sizeof(bits));
        uint64_t flags = interrupt_save();
        for (unsigned key = 0; key < INPUT_KEY_STATE_SIZE; key++)
            if (key_down[key]) bitmap_set(bits, sizeof(bits), key);
        interrupt_restore(flags);
        return evdev_copy_out(user_argument, bits, size, size);
    }
    if (nr == EVIOCGLED_NR || nr == EVIOCGSND_NR || nr == EVIOCGSW_NR) {
        memset(bits, 0, sizeof(bits));
        return evdev_copy_out(user_argument, bits, size, size);
    }

    /* There are no absolute axes, so there is no axis to describe. */
    if (nr >= EVIOCGABS_NR && nr < EVIOCGABS_NR + 0x40U) return -EINVAL;

    if (nr == EVIOCSCLOCKID_NR) {
        int32_t clock_id = 0;
        if (input_copy_from_user(&clock_id, user_argument, sizeof(clock_id)) != 0)
            return -EFAULT;
        if (clock_id != EVDEV_CLOCK_REALTIME && clock_id != EVDEV_CLOCK_MONOTONIC)
            return -EINVAL;
        if (reader) reader->clock_id = clock_id;
        return 0;
    }

    if (nr == EVIOCGRAB_NR) {
        /* The argument is a flag, not a pointer: non-zero grabs. */
        if (reader) {
            reader->grabbed = user_argument != 0;
            /* The console saw part of a key sequence before ownership changed
               hands, or will have missed the release of a held modifier while
               it was grabbed. Either way its idea of what is held is stale. */
            tty_reset_keyboard_state();
        }
        return 0;
    }

    /* Revoking is seatd handing the device back on session switch. There are no
       sessions to switch between, but refusing makes seatd log an error on
       every close, so the request is accepted and simply drains the queue. */
    if (nr == EVIOCREVOKE_NR) {
        if (reader) {
            uint64_t flags = interrupt_save();
            reader->head = reader->tail = reader->count = 0;
            reader->grabbed = 0;
            interrupt_restore(flags);
        }
        return 0;
    }

    return -EINVAL;
}

int64_t input_reader_read(struct input_reader *reader, size_t size, void *buffer) {
    if (!reader || !buffer) return -EINVAL;
    if (size < sizeof(struct tunix_input_event)) return -EINVAL;
    input_poll();

    size_t event_capacity = size / sizeof(struct tunix_input_event);
    uint64_t flags = interrupt_save();
    if (!reader->count) {
        interrupt_restore(flags);
        return -EAGAIN;
    }

    /* Realtime is uptime plus the boot epoch; monotonic is uptime as-is. */
    uint64_t epoch_offset = 0;
    if (reader->clock_id == EVDEV_CLOCK_REALTIME) {
        uint64_t uptime = time_uptime_ns();
        uint64_t now = time_epoch_seconds() * 1000000000ULL;
        epoch_offset = now > uptime ? now - uptime : 0;
    }

    struct tunix_input_event *out = (struct tunix_input_event *)buffer;
    size_t completed = 0;
    while (completed < event_capacity && reader->count) {
        const struct input_record *record = &reader->events[reader->head];
        uint64_t stamp = record->time_ns + epoch_offset;
        out[completed].tv_sec = (int64_t)(stamp / 1000000000ULL);
        out[completed].tv_usec = (int64_t)((stamp % 1000000000ULL) / 1000ULL);
        out[completed].type = record->type;
        out[completed].code = record->code;
        out[completed].value = record->value;
        completed++;
        reader->head = (reader->head + 1U) % INPUT_READER_CAPACITY;
        reader->count--;
    }
    interrupt_restore(flags);
    return (int64_t)(completed * sizeof(struct tunix_input_event));
}
