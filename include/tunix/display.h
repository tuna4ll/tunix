#ifndef TUNIX_DISPLAY_H
#define TUNIX_DISPLAY_H

#include <stdint.h>

struct boot_framebuffer_info;

struct display_early_driver {
    const char *name;
    int (*setup)(const struct boot_framebuffer_info *boot, struct boot_framebuffer_info *out);
    void (*late)(void);
};

struct display_flipper {
    uint8_t *(*back_buffer)(void);
    int (*flip_to_back)(void);
    int (*flip_to_console)(void);
    int (*console_in_front)(void);
    int (*idle)(int may_service);
    int (*vblank)(uint64_t *sequence, uint64_t *time_ns);
    // The 128-byte EDID of the monitor in front, or NULL when none was read.
    const uint8_t *(*edid)(void);
    // A 64x64 A8R8G8B8 cursor IMAGE shown over the screen, NULL to hide it,
    // and where its top left corner is; NULL when there is no cursor.
    int (*cursor_set)(const uint32_t *image);
    int (*cursor_move)(int32_t x, int32_t y);
    // A 256-entry gamma ramp, NULLs for none; NULL when there is no LUT.
    int (*gamma_set)(const uint16_t *red, const uint16_t *green, const uint16_t *blue);
};

int display_early_init(const struct boot_framebuffer_info *boot, struct boot_framebuffer_info *out);
void display_late_init(void);
void display_register_flipper(const struct display_flipper *flipper);
const struct display_flipper *display_flipper(void);

#endif
