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

static const struct display_early_driver *active_driver;
static const struct display_flipper *active_flipper;

int display_early_init(const struct boot_framebuffer_info *boot,
                       struct boot_framebuffer_info *out) {
    if (!boot || !out) return -1;
    for (size_t index = 0; early_drivers[index]; index++) {
        if (early_drivers[index]->setup(boot, out) != 0) continue;
        kprintf("DISPLAY: %s drives the console at %ux%u\n", early_drivers[index]->name,
                (unsigned)out->width, (unsigned)out->height);
        active_driver = early_drivers[index];
        return 0;
    }
    return -1;
}

void display_late_init(void) {
    if (active_driver && active_driver->late) active_driver->late();
}

void display_register_flipper(const struct display_flipper *flipper) { active_flipper = flipper; }

const struct display_flipper *display_flipper(void) { return active_flipper; }
