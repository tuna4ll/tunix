#ifndef TUNIX_INPUT_EVENT_ABI_H
#define TUNIX_INPUT_EVENT_ABI_H

#include <stdint.h>

#define TUNIX_INPUT_ABI_VERSION 1U

#define TUNIX_INPUT_DEVICE_KEYBOARD 0U
#define TUNIX_INPUT_DEVICE_MOUSE    1U

#define TUNIX_INPUT_NAME_MAX 32U
#define TUNIX_EVIOCGINFO 0x54490001UL

#define TUNIX_INPUT_CAP_KEYBOARD      (1U << 0)
#define TUNIX_INPUT_CAP_POINTER       (1U << 1)
#define TUNIX_INPUT_CAP_WHEEL         (1U << 2)
#define TUNIX_INPUT_CAP_EXTRA_BUTTONS (1U << 3)

struct tunix_input_device_info {
    uint32_t abi_version;
    uint32_t device_id;
    uint32_t event_types;
    uint32_t relative_axes;
    uint32_t capabilities;
    char name[TUNIX_INPUT_NAME_MAX];
};

/* Event types and synchronization codes intentionally follow Linux evdev. */
#define TUNIX_EV_SYN 0x00U
#define TUNIX_EV_KEY 0x01U
#define TUNIX_EV_REL 0x02U

#define TUNIX_SYN_REPORT  0U
#define TUNIX_SYN_DROPPED 3U

#define TUNIX_REL_X     0U
#define TUNIX_REL_Y     1U
#define TUNIX_REL_WHEEL 8U

#define TUNIX_KEY_RESERVED   0U
#define TUNIX_KEY_ESC        1U
#define TUNIX_KEY_1          2U
#define TUNIX_KEY_2          3U
#define TUNIX_KEY_3          4U
#define TUNIX_KEY_4          5U
#define TUNIX_KEY_5          6U
#define TUNIX_KEY_6          7U
#define TUNIX_KEY_7          8U
#define TUNIX_KEY_8          9U
#define TUNIX_KEY_9          10U
#define TUNIX_KEY_0          11U
#define TUNIX_KEY_MINUS      12U
#define TUNIX_KEY_EQUAL      13U
#define TUNIX_KEY_BACKSPACE  14U
#define TUNIX_KEY_TAB        15U
#define TUNIX_KEY_Q          16U
#define TUNIX_KEY_W          17U
#define TUNIX_KEY_E          18U
#define TUNIX_KEY_R          19U
#define TUNIX_KEY_T          20U
#define TUNIX_KEY_Y          21U
#define TUNIX_KEY_U          22U
#define TUNIX_KEY_I          23U
#define TUNIX_KEY_O          24U
#define TUNIX_KEY_P          25U
#define TUNIX_KEY_LEFTBRACE  26U
#define TUNIX_KEY_RIGHTBRACE 27U
#define TUNIX_KEY_ENTER      28U
#define TUNIX_KEY_LEFTCTRL   29U
#define TUNIX_KEY_A          30U
#define TUNIX_KEY_S          31U
#define TUNIX_KEY_D          32U
#define TUNIX_KEY_F          33U
#define TUNIX_KEY_G          34U
#define TUNIX_KEY_H          35U
#define TUNIX_KEY_J          36U
#define TUNIX_KEY_K          37U
#define TUNIX_KEY_L          38U
#define TUNIX_KEY_SEMICOLON  39U
#define TUNIX_KEY_APOSTROPHE 40U
#define TUNIX_KEY_GRAVE      41U
#define TUNIX_KEY_LEFTSHIFT  42U
#define TUNIX_KEY_BACKSLASH  43U
#define TUNIX_KEY_Z          44U
#define TUNIX_KEY_X          45U
#define TUNIX_KEY_C          46U
#define TUNIX_KEY_V          47U
#define TUNIX_KEY_B          48U
#define TUNIX_KEY_N          49U
#define TUNIX_KEY_M          50U
#define TUNIX_KEY_COMMA      51U
#define TUNIX_KEY_DOT        52U
#define TUNIX_KEY_SLASH      53U
#define TUNIX_KEY_RIGHTSHIFT 54U
#define TUNIX_KEY_KPASTERISK 55U
#define TUNIX_KEY_LEFTALT    56U
#define TUNIX_KEY_SPACE      57U
#define TUNIX_KEY_CAPSLOCK   58U
#define TUNIX_KEY_F1         59U
#define TUNIX_KEY_F2         60U
#define TUNIX_KEY_F3         61U
#define TUNIX_KEY_F4         62U
#define TUNIX_KEY_F5         63U
#define TUNIX_KEY_F6         64U
#define TUNIX_KEY_F7         65U
#define TUNIX_KEY_F8         66U
#define TUNIX_KEY_F9         67U
#define TUNIX_KEY_F10        68U
#define TUNIX_KEY_NUMLOCK    69U
#define TUNIX_KEY_SCROLLLOCK 70U
#define TUNIX_KEY_KP7        71U
#define TUNIX_KEY_KP8        72U
#define TUNIX_KEY_KP9        73U
#define TUNIX_KEY_KPMINUS    74U
#define TUNIX_KEY_KP4        75U
#define TUNIX_KEY_KP5        76U
#define TUNIX_KEY_KP6        77U
#define TUNIX_KEY_KPPLUS     78U
#define TUNIX_KEY_KP1        79U
#define TUNIX_KEY_KP2        80U
#define TUNIX_KEY_KP3        81U
#define TUNIX_KEY_KP0        82U
#define TUNIX_KEY_KPDOT      83U
#define TUNIX_KEY_F11        87U
#define TUNIX_KEY_F12        88U
#define TUNIX_KEY_KPENTER    96U
#define TUNIX_KEY_RIGHTCTRL  97U
#define TUNIX_KEY_KPSLASH    98U
#define TUNIX_KEY_SYSRQ      99U
#define TUNIX_KEY_RIGHTALT   100U
#define TUNIX_KEY_HOME       102U
#define TUNIX_KEY_UP         103U
#define TUNIX_KEY_PAGEUP     104U
#define TUNIX_KEY_LEFT       105U
#define TUNIX_KEY_RIGHT      106U
#define TUNIX_KEY_END        107U
#define TUNIX_KEY_DOWN       108U
#define TUNIX_KEY_PAGEDOWN   109U
#define TUNIX_KEY_INSERT     110U
#define TUNIX_KEY_DELETE     111U
#define TUNIX_KEY_PAUSE      119U
#define TUNIX_KEY_LEFTMETA   125U
#define TUNIX_KEY_RIGHTMETA  126U
#define TUNIX_KEY_COMPOSE    127U

#define TUNIX_BTN_LEFT   0x110U
#define TUNIX_BTN_RIGHT  0x111U
#define TUNIX_BTN_MIDDLE 0x112U
#define TUNIX_BTN_SIDE   0x113U
#define TUNIX_BTN_EXTRA  0x114U

/*
 * The record /dev/input/event* delivers. This is Linux's `struct input_event`
 * exactly -- 64-bit timeval followed by type/code/value -- and it has to stay
 * that way: libinput reads it straight off the descriptor into its own
 * definition, and a layout of our own would be silently misparsed rather than
 * rejected.
 *
 * The timestamp's clock is per-descriptor and selected with EVIOCSCLOCKID;
 * it is the realtime clock until a reader asks for CLOCK_MONOTONIC, which
 * libinput does immediately.
 */
struct tunix_input_event {
    int64_t tv_sec;
    int64_t tv_usec;
    uint16_t type;
    uint16_t code;
    int32_t value;
};

_Static_assert(sizeof(struct tunix_input_event) == 24U,
               "the input event must stay Linux's struct input_event");

#endif
