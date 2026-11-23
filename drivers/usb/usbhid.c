// USB HID: keyboards, mice and tablets, as a class driver on the USB device model. The
// report descriptor parser decides whether a non-boot interface is a pointer it can
// read; reports become Tunix input events.

#include <stddef.h>
#include <stdint.h>

#include <tunix/heap.h>
#include <tunix/input.h>
#include <tunix/kprintf.h>
#include <tunix/kstring.h>
#include <tunix/module.h>
#include <tunix/usb.h>
#include <uapi/input_event.h>

#define HID_KEYBOARD_REPORT_BYTES 8U

struct hid_field {
    uint16_t offset;
    uint8_t size;
    uint8_t is_signed;
};

struct hid_mouse_layout {
    uint8_t report_id;
    uint8_t buttons;
    uint16_t buttons_offset;
    struct hid_field x;
    struct hid_field y;
    struct hid_field wheel;
    struct hid_field pan;
    uint16_t report_bits;
    uint8_t absolute;
    int32_t x_max;
    int32_t y_max;
};

// ---- report descriptors ----

#define ITEM_MAIN   0U
#define ITEM_GLOBAL 1U
#define ITEM_LOCAL  2U

#define MAIN_INPUT          0x8U
#define MAIN_OUTPUT         0x9U
#define MAIN_COLLECTION     0xAU
#define MAIN_FEATURE        0xBU
#define MAIN_END_COLLECTION 0xCU

#define GLOBAL_USAGE_PAGE      0x0U
#define GLOBAL_LOGICAL_MINIMUM 0x1U
#define GLOBAL_LOGICAL_MAXIMUM 0x2U
#define GLOBAL_REPORT_SIZE     0x7U
#define GLOBAL_REPORT_ID       0x8U
#define GLOBAL_REPORT_COUNT    0x9U
#define GLOBAL_PUSH            0xAU
#define GLOBAL_POP             0xBU

#define LOCAL_USAGE         0x0U
#define LOCAL_USAGE_MINIMUM 0x1U
#define LOCAL_USAGE_MAXIMUM 0x2U

#define INPUT_CONSTANT 0x1U
#define INPUT_RELATIVE 0x4U

#define PAGE_GENERIC_DESKTOP   0x01U
#define PAGE_BUTTON            0x09U
#define PAGE_CONSUMER          0x0CU
#define USAGE_POINTER          0x01U
#define USAGE_MOUSE            0x02U
#define USAGE_X                0x30U
#define USAGE_Y                0x31U
#define USAGE_WHEEL            0x38U
#define USAGE_AC_PAN           0x238U
#define COLLECTION_APPLICATION 1U

#define MAX_USAGES 32U
#define MAX_STACK  4U
#define MAX_DEPTH  16U

struct globals {
    uint32_t usage_page;
    int32_t logical_minimum;
    int32_t logical_maximum;
    uint32_t report_size;
    uint32_t report_count;
    uint32_t report_id;
};

static int32_t sign_extend(uint32_t value, uint32_t bytes) {
    if (bytes == 1U) return (int8_t)value;
    if (bytes == 2U) return (int16_t)value;
    return (int32_t)value;
}

static void set_field(struct hid_field *field, uint32_t offset, uint32_t size, int is_signed) {
    if (field->size) return;
    field->offset = (uint16_t)offset;
    field->size = (uint8_t)(size > 32U ? 32U : size);
    field->is_signed = (uint8_t)(is_signed ? 1 : 0);
}

static int hid_parse_mouse(const uint8_t *descriptor, uint32_t length, struct hid_mouse_layout *out) {
    struct globals global = {0, 0, 0, 0, 0, 0};
    struct globals stack[MAX_STACK];
    unsigned stack_depth = 0;
    uint32_t usages[MAX_USAGES];
    unsigned usage_count = 0;
    uint32_t usage_minimum = 0, usage_maximum = 0;
    int have_range = 0;
    uint16_t bits[256];
    int mouse_depth = -1;
    int depth = 0;
    struct hid_mouse_layout found;
    int chosen = -1;

    for (unsigned index = 0; index < 256U; index++) bits[index] = 0;
    for (unsigned index = 0; index < sizeof(found); index++) ((uint8_t *)&found)[index] = 0;

    for (uint32_t at = 0; at < length;) {
        uint8_t prefix = descriptor[at];
        if (prefix == 0xFEU) {
            if (at + 2U >= length) break;
            at += 3U + descriptor[at + 1U];
            continue;
        }
        uint32_t size = prefix & 3U;
        if (size == 3U) size = 4U;
        uint32_t type = (prefix >> 2) & 3U;
        uint32_t tag = prefix >> 4;
        if (at + 1U + size > length) break;
        uint32_t data = 0;
        for (uint32_t byte = 0; byte < size; byte++)
            data |= (uint32_t)descriptor[at + 1U + byte] << (8U * byte);
        at += 1U + size;

        if (type == ITEM_GLOBAL) {
            switch (tag) {
            case GLOBAL_USAGE_PAGE:      global.usage_page = data; break;
            case GLOBAL_LOGICAL_MINIMUM: global.logical_minimum = sign_extend(data, size); break;
            case GLOBAL_LOGICAL_MAXIMUM:
                global.logical_maximum =
                    global.logical_minimum < 0 ? sign_extend(data, size) : (int32_t)data;
                break;
            case GLOBAL_REPORT_SIZE:  global.report_size = data; break;
            case GLOBAL_REPORT_COUNT: global.report_count = data; break;
            case GLOBAL_REPORT_ID:    global.report_id = data & 0xFFU; break;
            case GLOBAL_PUSH:
                if (stack_depth < MAX_STACK) stack[stack_depth++] = global;
                break;
            case GLOBAL_POP:
                if (stack_depth) global = stack[--stack_depth];
                break;
            default: break;
            }
            continue;
        }
        if (type == ITEM_LOCAL) {
            uint32_t usage = size == 4U ? data : (global.usage_page << 16) | data;
            if (tag == LOCAL_USAGE && usage_count < MAX_USAGES) usages[usage_count++] = usage;
            if (tag == LOCAL_USAGE_MINIMUM) {
                usage_minimum = usage;
                have_range = 1;
            }
            if (tag == LOCAL_USAGE_MAXIMUM) {
                usage_maximum = usage;
                have_range = 1;
            }
            continue;
        }
        if (type != ITEM_MAIN) continue;

        if (tag == MAIN_COLLECTION) {
            uint32_t usage = usage_count ? usages[0] : 0;
            if (mouse_depth < 0 && data == COLLECTION_APPLICATION &&
                (usage == ((PAGE_GENERIC_DESKTOP << 16) | USAGE_MOUSE) ||
                 usage == ((PAGE_GENERIC_DESKTOP << 16) | USAGE_POINTER)))
                mouse_depth = depth;
            if (depth < (int)MAX_DEPTH) depth++;
        } else if (tag == MAIN_END_COLLECTION) {
            if (depth > 0) depth--;
            if (mouse_depth == depth) mouse_depth = -1;
        } else if (tag == MAIN_INPUT) {
            uint32_t id = global.report_id;
            uint32_t offset = bits[id];
            uint32_t total = global.report_size * global.report_count;
            if (mouse_depth >= 0 && !(data & INPUT_CONSTANT) && global.report_size &&
                (chosen < 0 || (uint32_t)chosen == id)) {
                for (uint32_t item = 0; item < global.report_count && item < 64U; item++) {
                    uint32_t usage;
                    if (have_range) {
                        usage = usage_minimum + item;
                        if (usage_maximum && usage > usage_maximum) break;
                    } else if (usage_count) {
                        usage = usages[item < usage_count ? item : usage_count - 1U];
                    } else {
                        break;
                    }
                    uint32_t page = usage >> 16;
                    uint32_t code = usage & 0xFFFFU;
                    uint32_t field = offset + item * global.report_size;
                    int is_signed = global.logical_minimum < 0;
                    if (page == PAGE_BUTTON && code >= 1U && code <= 32U) {
                        if (!found.buttons) {
                            found.buttons_offset = (uint16_t)(field - (code - 1U));
                            chosen = (int)id;
                        }
                        if (code > found.buttons) found.buttons = (uint8_t)code;
                    } else if (page == PAGE_GENERIC_DESKTOP && (data & INPUT_RELATIVE)) {
                        if (code == USAGE_X)
                            set_field(&found.x, field, global.report_size, is_signed);
                        if (code == USAGE_Y)
                            set_field(&found.y, field, global.report_size, is_signed);
                        if (code == USAGE_WHEEL)
                            set_field(&found.wheel, field, global.report_size, is_signed);
                        if (code == USAGE_X || code == USAGE_Y) chosen = (int)id;
                    } else if (page == PAGE_GENERIC_DESKTOP &&
                               (code == USAGE_X || code == USAGE_Y) &&
                               (!found.x.size || found.absolute)) {
                        if (code == USAGE_X) {
                            set_field(&found.x, field, global.report_size, is_signed);
                            found.x_max = global.logical_maximum;
                        } else {
                            set_field(&found.y, field, global.report_size, is_signed);
                            found.y_max = global.logical_maximum;
                        }
                        found.absolute = 1;
                        chosen = (int)id;
                    } else if (page == PAGE_CONSUMER && code == USAGE_AC_PAN &&
                               (data & INPUT_RELATIVE)) {
                        set_field(&found.pan, field, global.report_size, is_signed);
                    }
                }
            }
            bits[id] = (uint16_t)(offset + total);
        }
        usage_count = 0;
        have_range = 0;
        usage_minimum = 0;
        usage_maximum = 0;
    }

    if (!found.x.size || !found.y.size || chosen < 0) return -1;
    found.report_id = (uint8_t)chosen;
    found.report_bits = bits[chosen];
    *out = found;
    return 0;
}

static int32_t extract(const uint8_t *report, uint32_t length, const struct hid_field *field) {
    if (!field->size) return 0;
    uint32_t end = (uint32_t)field->offset + field->size;
    if ((end + 7U) / 8U > length) return 0;
    uint64_t value = 0;
    for (uint32_t bit = 0; bit < field->size; bit++) {
        uint32_t position = field->offset + bit;
        if (report[position / 8U] & (1U << (position % 8U))) value |= 1ULL << bit;
    }
    if (field->is_signed && field->size < 64U && (value & (1ULL << (field->size - 1U))))
        value |= ~0ULL << field->size;
    return (int32_t)(int64_t)value;
}

static int hid_decode_mouse(const struct hid_mouse_layout *layout, const uint8_t *report, uint32_t length,
                     int *dx, int *dy, int *wheel, uint32_t *buttons) {
    if (layout->report_id) {
        if (!length || report[0] != layout->report_id) return -1;
        report++;
        length--;
    }
    if (length * 8U < layout->y.offset + layout->y.size) return -1;
    *dx = extract(report, length, &layout->x);
    *dy = extract(report, length, &layout->y);
    *wheel = extract(report, length, &layout->wheel);
    uint32_t pressed = 0;
    for (uint32_t button = 0; button < layout->buttons && button < 32U; button++) {
        uint32_t position = (uint32_t)layout->buttons_offset + button;
        if ((position + 8U) / 8U > length) break;
        if (report[position / 8U] & (1U << (position % 8U))) pressed |= 1U << button;
    }
    *buttons = pressed;
    return 0;
}

// ---- reports to input events ----

#define KEYBOARD_REPORT_KEYS        6U
#define KEYBOARD_REPORT_KEYS_OFFSET 2U
#define KEYBOARD_MODIFIER_COUNT     8U
#define HID_USAGE_FIRST_KEY         4U
#define HID_ERROR_ROLLOVER          1U
#define MOUSE_BUTTON_MASK           0x07U

static uint16_t keycode_for_usage(uint8_t usage) {
    static const uint16_t letters[] = {
        TUNIX_KEY_A, TUNIX_KEY_B, TUNIX_KEY_C, TUNIX_KEY_D, TUNIX_KEY_E, TUNIX_KEY_F, TUNIX_KEY_G,
        TUNIX_KEY_H, TUNIX_KEY_I, TUNIX_KEY_J, TUNIX_KEY_K, TUNIX_KEY_L, TUNIX_KEY_M, TUNIX_KEY_N,
        TUNIX_KEY_O, TUNIX_KEY_P, TUNIX_KEY_Q, TUNIX_KEY_R, TUNIX_KEY_S, TUNIX_KEY_T, TUNIX_KEY_U,
        TUNIX_KEY_V, TUNIX_KEY_W, TUNIX_KEY_X, TUNIX_KEY_Y, TUNIX_KEY_Z,
    };
    static const uint16_t digits[] = {
        TUNIX_KEY_1, TUNIX_KEY_2, TUNIX_KEY_3, TUNIX_KEY_4, TUNIX_KEY_5,
        TUNIX_KEY_6, TUNIX_KEY_7, TUNIX_KEY_8, TUNIX_KEY_9, TUNIX_KEY_0,
    };
    static const uint16_t punctuation[] = {
        TUNIX_KEY_ENTER,      TUNIX_KEY_ESC,       TUNIX_KEY_BACKSPACE, TUNIX_KEY_TAB,
        TUNIX_KEY_SPACE,      TUNIX_KEY_MINUS,     TUNIX_KEY_EQUAL,     TUNIX_KEY_LEFTBRACE,
        TUNIX_KEY_RIGHTBRACE, TUNIX_KEY_BACKSLASH, TUNIX_KEY_RESERVED,  TUNIX_KEY_SEMICOLON,
        TUNIX_KEY_APOSTROPHE, TUNIX_KEY_GRAVE,     TUNIX_KEY_COMMA,     TUNIX_KEY_DOT,
        TUNIX_KEY_SLASH,      TUNIX_KEY_CAPSLOCK,
    };
    static const uint16_t function_keys[] = {
        TUNIX_KEY_F1, TUNIX_KEY_F2, TUNIX_KEY_F3, TUNIX_KEY_F4,  TUNIX_KEY_F5,  TUNIX_KEY_F6,
        TUNIX_KEY_F7, TUNIX_KEY_F8, TUNIX_KEY_F9, TUNIX_KEY_F10, TUNIX_KEY_F11, TUNIX_KEY_F12,
    };

    if (usage >= 0x04U && usage <= 0x1DU) return letters[usage - 0x04U];
    if (usage >= 0x1EU && usage <= 0x27U) return digits[usage - 0x1EU];
    if (usage >= 0x28U && usage <= 0x39U) return punctuation[usage - 0x28U];
    if (usage >= 0x3AU && usage <= 0x45U) return function_keys[usage - 0x3AU];
    switch (usage) {
    case 0x46U: return TUNIX_KEY_SYSRQ;
    case 0x47U: return TUNIX_KEY_SCROLLLOCK;
    case 0x48U: return TUNIX_KEY_PAUSE;
    case 0x49U: return TUNIX_KEY_INSERT;
    case 0x4AU: return TUNIX_KEY_HOME;
    case 0x4BU: return TUNIX_KEY_PAGEUP;
    case 0x4CU: return TUNIX_KEY_DELETE;
    case 0x4DU: return TUNIX_KEY_END;
    case 0x4EU: return TUNIX_KEY_PAGEDOWN;
    case 0x4FU: return TUNIX_KEY_RIGHT;
    case 0x50U: return TUNIX_KEY_LEFT;
    case 0x51U: return TUNIX_KEY_DOWN;
    case 0x52U: return TUNIX_KEY_UP;
    case 0x53U: return TUNIX_KEY_NUMLOCK;
    default:    return TUNIX_KEY_RESERVED;
    }
}

static uint16_t keycode_for_modifier(unsigned bit) {
    static const uint16_t modifiers[KEYBOARD_MODIFIER_COUNT] = {
        TUNIX_KEY_LEFTCTRL,  TUNIX_KEY_LEFTSHIFT,  TUNIX_KEY_LEFTALT,  TUNIX_KEY_LEFTMETA,
        TUNIX_KEY_RIGHTCTRL, TUNIX_KEY_RIGHTSHIFT, TUNIX_KEY_RIGHTALT, TUNIX_KEY_RIGHTMETA,
    };
    return modifiers[bit];
}

static void keyboard_apply(uint8_t *previous, const uint8_t *now_report) {
    for (unsigned bit = 0; bit < KEYBOARD_MODIFIER_COUNT; bit++) {
        uint8_t mask = (uint8_t)(1U << bit);
        if ((now_report[0] & mask) == (previous[0] & mask)) continue;
        input_external_key(keycode_for_modifier(bit), (now_report[0] & mask) ? 0 : 1);
    }
    for (unsigned i = 0; i < KEYBOARD_REPORT_KEYS; i++) {
        uint8_t usage = previous[KEYBOARD_REPORT_KEYS_OFFSET + i];
        if (usage < HID_USAGE_FIRST_KEY) continue;
        int still_held = 0;
        for (unsigned j = 0; j < KEYBOARD_REPORT_KEYS; j++)
            if (now_report[KEYBOARD_REPORT_KEYS_OFFSET + j] == usage) still_held = 1;
        if (!still_held) input_external_key(keycode_for_usage(usage), 1);
    }
    for (unsigned i = 0; i < KEYBOARD_REPORT_KEYS; i++) {
        uint8_t usage = now_report[KEYBOARD_REPORT_KEYS_OFFSET + i];
        if (usage < HID_USAGE_FIRST_KEY) continue;
        int was_held = 0;
        for (unsigned j = 0; j < KEYBOARD_REPORT_KEYS; j++)
            if (previous[KEYBOARD_REPORT_KEYS_OFFSET + j] == usage) was_held = 1;
        if (!was_held) input_external_key(keycode_for_usage(usage), 0);
    }
    memcpy(previous, now_report, HID_KEYBOARD_REPORT_BYTES);
}

static void hid_keyboard_report(uint8_t *previous, const uint8_t *report, uint32_t length) {
    if (length < 3U) return;
    uint8_t now_report[HID_KEYBOARD_REPORT_BYTES];
    memset(now_report, 0, sizeof(now_report));
    memcpy(now_report, report,
           length < HID_KEYBOARD_REPORT_BYTES ? length : HID_KEYBOARD_REPORT_BYTES);
    if (now_report[KEYBOARD_REPORT_KEYS_OFFSET] == HID_ERROR_ROLLOVER) return;
    keyboard_apply(previous, now_report);
}

static void hid_keyboard_release(uint8_t *previous) {
    uint8_t released[HID_KEYBOARD_REPORT_BYTES];
    memset(released, 0, sizeof(released));
    keyboard_apply(previous, released);
}

static void hid_mouse_report(const struct hid_mouse_layout *layout, const uint8_t *report,
                      uint32_t length) {
    if (layout) {
        int dx, dy, wheel;
        uint32_t buttons;
        if (hid_decode_mouse(layout, report, length, &dx, &dy, &wheel, &buttons) != 0) return;
        if (layout->absolute) {
            input_external_tablet(dx, dy, layout->x_max, layout->y_max, wheel,
                                  (uint8_t)(buttons & MOUSE_BUTTON_MASK));
            return;
        }
        input_external_mouse(dx, dy, wheel, (uint8_t)(buttons & MOUSE_BUTTON_MASK));
        return;
    }
    if (length < 3U) return;
    int wheel = length >= 4U ? (int)(int8_t)report[3] : 0;
    input_external_mouse((int)(int8_t)report[1], (int)(int8_t)report[2], wheel,
                         report[0] & MOUSE_BUTTON_MASK);
}

// ---- the driver ----

#define CLASS_HID                    0x03U
#define HID_SUBCLASS_BOOT            1U
#define HID_BOOT_KEYBOARD            1U
#define HID_BOOT_MOUSE               2U
#define REQUEST_TYPE_CLASS_INTERFACE 0x21U
#define REQUEST_TYPE_INTERFACE_IN    0x81U
#define REQUEST_GET_DESCRIPTOR       0x06U
#define REQUEST_HID_SET_IDLE         0x0AU
#define REQUEST_HID_SET_PROTOCOL     0x0BU
#define DESCRIPTOR_HID_REPORT        0x22U
#define REPORT_DESCRIPTOR_BYTES      1024U

struct hid_interface {
    int keyboard;
    int report_mode;
    int pointer;
    struct hid_mouse_layout layout;
    uint8_t previous[HID_KEYBOARD_REPORT_BYTES];
};

static void hid_report(void *context, const uint8_t *data, uint32_t length) {
    struct hid_interface *state = context;
    if (state->keyboard) hid_keyboard_report(state->previous, data, length);
    else hid_mouse_report(state->report_mode ? &state->layout : NULL, data, length);
}

static int usbhid_probe(struct usb_dev *device, unsigned index) {
    const struct usb_interface_info *interface = &device->interfaces[index];
    const struct usb_host_ops *ops = device->ops;
    if (!interface->interrupt_in) return -1;
    struct hid_interface *state = kmalloc(sizeof(*state));
    if (!state) return -1;
    memset(state, 0, sizeof(*state));

    if (interface->subclass == HID_SUBCLASS_BOOT && interface->protocol == HID_BOOT_KEYBOARD) {
        state->keyboard = 1;
    } else {
        uint16_t length = interface->hid_report_length;
        if (!length || length > REPORT_DESCRIPTOR_BYTES) length = REPORT_DESCRIPTOR_BYTES;
        uint8_t *descriptor = kmalloc(REPORT_DESCRIPTOR_BYTES);
        if (!descriptor) {
            kfree(state);
            return -1;
        }
        memset(descriptor, 0, REPORT_DESCRIPTOR_BYTES);
        if (ops->control(device->host_device, REQUEST_TYPE_INTERFACE_IN, REQUEST_GET_DESCRIPTOR,
                         DESCRIPTOR_HID_REPORT << 8, interface->number, descriptor, length) == 0 &&
            hid_parse_mouse(descriptor, length, &state->layout) == 0)
            state->report_mode = 1;
        kfree(descriptor);
        if (!state->report_mode &&
            !(interface->subclass == HID_SUBCLASS_BOOT && interface->protocol == HID_BOOT_MOUSE)) {
            kprintf("usbhid: %s interface %u is HID but not a keyboard or a pointer\n",
                    device->name, (unsigned)interface->number);
            kfree(state);
            return -1;
        }
    }

    // Boot devices speak the boot protocol unless the report descriptor was understood.
    if (!state->report_mode || interface->subclass == HID_SUBCLASS_BOOT)
        (void)ops->control(device->host_device, REQUEST_TYPE_CLASS_INTERFACE,
                           REQUEST_HID_SET_PROTOCOL, state->report_mode ? 1U : 0U,
                           interface->number, NULL, 0);
    (void)ops->control(device->host_device, REQUEST_TYPE_CLASS_INTERFACE, REQUEST_HID_SET_IDLE, 0,
                       interface->number, NULL, 0);
    if (ops->interrupt_open(device->host_device, interface, hid_report, state) != 0) {
        kprintf("usbhid: %s interface %u: the endpoint would not open\n", device->name,
                (unsigned)interface->number);
        kfree(state);
        return -1;
    }
    device->driver_data[index] = state;
    if (!state->keyboard) {
        state->pointer = 1;
        input_usb_pointer(1);
    }
    kprintf("usbhid: %s interface %u: %s\n", device->name, (unsigned)interface->number,
            state->keyboard        ? "keyboard"
                : !state->report_mode ? "boot mouse"
                : state->layout.absolute ? "tablet"
                                         : "mouse");
    return 0;
}

static void usbhid_disconnect(struct usb_dev *device, unsigned index) {
    struct hid_interface *state = device->driver_data[index];
    if (!state) return;
    device->ops->interrupt_close(device->host_device, &device->interfaces[index]);
    if (state->keyboard) hid_keyboard_release(state->previous);
    if (state->pointer) input_usb_pointer(-1);
    kfree(state);
    device->driver_data[index] = NULL;
}

static const struct usb_device_id usbhid_ids[] = {
    {CLASS_HID, USB_ANY, USB_ANY},
};

static struct usb_driver usbhid_driver = {
    .name = "usbhid",
    .ids = usbhid_ids,
    .id_count = sizeof(usbhid_ids) / sizeof(usbhid_ids[0]),
    .probe = usbhid_probe,
    .disconnect = usbhid_disconnect,
};

static int usbhid_load(void) { return usb_register_driver(&usbhid_driver); }

static void usbhid_unload(void) { usb_unregister_driver(&usbhid_driver); }

MODULE_MAIN(usbhid_load, usbhid_unload);
MODULE_ALIAS("usb:v*p*d*dc*dsc*dp*ic03isc*ip*in*");
MODULE_LICENSE("MIT");
MODULE_DESCRIPTION("USB keyboards, mice and tablets");
MODULE_AUTHOR("Tunix");
