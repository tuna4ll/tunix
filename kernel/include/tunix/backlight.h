#ifndef TUNIX_BACKLIGHT_H
#define TUNIX_BACKLIGHT_H

#include <stdint.h>

struct pci_device;

struct backlight_device {
    const char *name;
    const char *type;
    uint32_t max_brightness;
    uint32_t (*get)(void);
    int (*set)(uint32_t brightness);
};

void sysfs_publish_backlight(const struct backlight_device *device,
                             const struct pci_device *parent);

#endif
