#ifndef TUNIX_DISPLAY_H
#define TUNIX_DISPLAY_H

struct boot_framebuffer_info;

struct display_early_driver {
    const char *name;
    int (*setup)(const struct boot_framebuffer_info *boot, struct boot_framebuffer_info *out);
};

int display_early_init(const struct boot_framebuffer_info *boot, struct boot_framebuffer_info *out);

#endif
