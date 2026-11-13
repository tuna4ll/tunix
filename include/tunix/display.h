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
};

int display_early_init(const struct boot_framebuffer_info *boot, struct boot_framebuffer_info *out);
void display_late_init(void);
void display_register_flipper(const struct display_flipper *flipper);
const struct display_flipper *display_flipper(void);

#endif
