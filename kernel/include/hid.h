#ifndef TUNIX_HID_H
#define TUNIX_HID_H

#include <stdint.h>

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
};

int hid_parse_mouse(const uint8_t *descriptor, uint32_t length, struct hid_mouse_layout *out);
int hid_decode_mouse(const struct hid_mouse_layout *layout, const uint8_t *report,
                     uint32_t length, int *dx, int *dy, int *wheel, uint32_t *buttons);

#endif
