#include <stdint.h>

#include "../../include/hid.h"
#include "../../include/hid_input.h"
#include "../../include/input.h"
#include "../../include/kstring.h"
#include "../../include/tunix/input_event.h"

#define KEYBOARD_REPORT_KEYS 6U
#define KEYBOARD_REPORT_KEYS_OFFSET 2U
#define KEYBOARD_MODIFIER_COUNT 8U
#define HID_USAGE_FIRST_KEY 4U
#define HID_ERROR_ROLLOVER 1U
#define MOUSE_BUTTON_MASK 0x07U

static uint16_t keycode_for_usage(uint8_t usage) {
    static const uint16_t letters[] = {
        TUNIX_KEY_A, TUNIX_KEY_B, TUNIX_KEY_C, TUNIX_KEY_D, TUNIX_KEY_E,
        TUNIX_KEY_F, TUNIX_KEY_G, TUNIX_KEY_H, TUNIX_KEY_I, TUNIX_KEY_J,
        TUNIX_KEY_K, TUNIX_KEY_L, TUNIX_KEY_M, TUNIX_KEY_N, TUNIX_KEY_O,
        TUNIX_KEY_P, TUNIX_KEY_Q, TUNIX_KEY_R, TUNIX_KEY_S, TUNIX_KEY_T,
        TUNIX_KEY_U, TUNIX_KEY_V, TUNIX_KEY_W, TUNIX_KEY_X, TUNIX_KEY_Y,
        TUNIX_KEY_Z,
    };
    static const uint16_t digits[] = {
        TUNIX_KEY_1, TUNIX_KEY_2, TUNIX_KEY_3, TUNIX_KEY_4, TUNIX_KEY_5,
        TUNIX_KEY_6, TUNIX_KEY_7, TUNIX_KEY_8, TUNIX_KEY_9, TUNIX_KEY_0,
    };
    static const uint16_t punctuation[] = {
        TUNIX_KEY_ENTER, TUNIX_KEY_ESC, TUNIX_KEY_BACKSPACE, TUNIX_KEY_TAB,
        TUNIX_KEY_SPACE, TUNIX_KEY_MINUS, TUNIX_KEY_EQUAL, TUNIX_KEY_LEFTBRACE,
        TUNIX_KEY_RIGHTBRACE, TUNIX_KEY_BACKSLASH, TUNIX_KEY_RESERVED,
        TUNIX_KEY_SEMICOLON, TUNIX_KEY_APOSTROPHE, TUNIX_KEY_GRAVE,
        TUNIX_KEY_COMMA, TUNIX_KEY_DOT, TUNIX_KEY_SLASH, TUNIX_KEY_CAPSLOCK,
    };
    static const uint16_t function_keys[] = {
        TUNIX_KEY_F1, TUNIX_KEY_F2, TUNIX_KEY_F3, TUNIX_KEY_F4, TUNIX_KEY_F5,
        TUNIX_KEY_F6, TUNIX_KEY_F7, TUNIX_KEY_F8, TUNIX_KEY_F9, TUNIX_KEY_F10,
        TUNIX_KEY_F11, TUNIX_KEY_F12,
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
        default: return TUNIX_KEY_RESERVED;
    }
}

static uint16_t keycode_for_modifier(unsigned bit) {
    static const uint16_t modifiers[KEYBOARD_MODIFIER_COUNT] = {
        TUNIX_KEY_LEFTCTRL, TUNIX_KEY_LEFTSHIFT, TUNIX_KEY_LEFTALT,
        TUNIX_KEY_LEFTMETA, TUNIX_KEY_RIGHTCTRL, TUNIX_KEY_RIGHTSHIFT,
        TUNIX_KEY_RIGHTALT, TUNIX_KEY_RIGHTMETA,
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

void hid_keyboard_report(uint8_t *previous, const uint8_t *report, uint32_t length) {
    if (length < 3U) return;
    uint8_t now_report[HID_KEYBOARD_REPORT_BYTES];
    memset(now_report, 0, sizeof(now_report));
    memcpy(now_report, report, length < HID_KEYBOARD_REPORT_BYTES ? length : HID_KEYBOARD_REPORT_BYTES);
    if (now_report[KEYBOARD_REPORT_KEYS_OFFSET] == HID_ERROR_ROLLOVER) return;
    keyboard_apply(previous, now_report);
}

void hid_keyboard_release(uint8_t *previous) {
    uint8_t released[HID_KEYBOARD_REPORT_BYTES];
    memset(released, 0, sizeof(released));
    keyboard_apply(previous, released);
}

void hid_mouse_report(const struct hid_mouse_layout *layout, const uint8_t *report, uint32_t length) {
    if (layout) {
        int dx, dy, wheel;
        uint32_t buttons;
        if (hid_decode_mouse(layout, report, length, &dx, &dy, &wheel, &buttons) != 0) return;
        input_external_mouse(dx, dy, wheel, (uint8_t)(buttons & MOUSE_BUTTON_MASK));
        return;
    }
    if (length < 3U) return;
    int wheel = length >= 4U ? (int)(int8_t)report[3] : 0;
    input_external_mouse((int)(int8_t)report[1], (int)(int8_t)report[2], wheel,
                         report[0] & MOUSE_BUTTON_MASK);
}
