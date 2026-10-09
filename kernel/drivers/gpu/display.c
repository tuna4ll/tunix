#include <stddef.h>

#include <tunix/boot_framebuffer.h>
#include <tunix/display.h>
#include <tunix/nv50.h>

extern void kprintf(const char *fmt, ...);

static const struct display_early_driver *const early_drivers[] = {
#if defined(__x86_64__)
    &nv50_display_driver,
#endif
    NULL,
};

int display_early_init(const struct boot_framebuffer_info *boot,
                       struct boot_framebuffer_info *out) {
    if (!boot || !out) return -1;
    for (size_t index = 0; early_drivers[index]; index++) {
        if (early_drivers[index]->setup(boot, out) != 0) continue;
        kprintf("DISPLAY: %s drives the console at %ux%u\n", early_drivers[index]->name,
                (unsigned)out->width, (unsigned)out->height);
        return 0;
    }
    return -1;
}
