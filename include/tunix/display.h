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
    // Copies ROWS of ROW_BYTES, PITCH apart, from system PAGES into the back
    // buffer and flips to it once copied (idle() says when); -1 when the
    // caller is to copy and flip itself. forget_pages is told when the
    // pages go. NULL when the driver has no engine to copy with.
    int (*present_pages)(const uint64_t *pages, uint64_t page_count, uint32_t pitch,
                         uint32_t row_bytes, uint32_t rows);
    void (*forget_pages)(const uint64_t *pages);
    // The same for a buffer in the device's own memory, at PHYSICAL as the
    // CPU sees it (a render driver's GEM buffer, in BAR1).
    // WIDTH is in pixels: a block-linear buffer's layout depends on it.
    int (*present_device)(uint64_t physical, uint32_t pitch, uint32_t width, uint32_t row_bytes,
                          uint32_t rows);
};

int display_early_init(const struct boot_framebuffer_info *boot, struct boot_framebuffer_info *out);
void display_late_init(void);
void display_register_flipper(const struct display_flipper *flipper);
const struct display_flipper *display_flipper(void);

#endif
