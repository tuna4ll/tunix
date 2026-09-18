#ifndef TUNIX_HID_INPUT_H
#define TUNIX_HID_INPUT_H

#include <stdint.h>

#include "hid.h"

#define HID_KEYBOARD_REPORT_BYTES 8U

void hid_keyboard_report(uint8_t *previous, const uint8_t *report, uint32_t length);
void hid_keyboard_release(uint8_t *previous);
void hid_mouse_report(const struct hid_mouse_layout *layout, const uint8_t *report, uint32_t length);

#endif
