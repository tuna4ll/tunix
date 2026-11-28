#include <stddef.h>

#include <tunix/boot_framebuffer.h>
#include <tunix/display.h>
#include <tunix/errno.h>
#include <tunix/kprintf.h>

// Display drivers that take the console from the firmware's framebuffer: a
// module from the initramfs registers itself before the console is drawn.
#define EARLY_DRIVERS 4U

static const struct display_early_driver *early_drivers[EARLY_DRIVERS];
static unsigned early_driver_count;
static int early_done;

static const struct display_early_driver *active_driver;
static const struct display_flipper *active_flipper;

int display_early_register(const struct display_early_driver *driver) {
    if (early_done) return -EBUSY;
    if (!driver || early_driver_count == EARLY_DRIVERS) return -ENOSPC;
    early_drivers[early_driver_count++] = driver;
    return 0;
}

int display_early_init(const struct boot_framebuffer_info *boot,
                       struct boot_framebuffer_info *out) {
    early_done = 1;
    if (!boot || !out) return -1;
    for (unsigned index = 0; index < early_driver_count; index++) {
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
