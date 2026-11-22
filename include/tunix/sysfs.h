#ifndef TUNIX_SYSFS_H
#define TUNIX_SYSFS_H

#include <stdint.h>

struct module;
struct pci_device;

void sysfs_init(void);
void sysfs_console_switched(void);
void sysfs_publish_cpus(unsigned cpus);
void sysfs_module_added(struct module *module);
void sysfs_module_removed(const char *name);
void sysfs_pci_bound(const struct pci_device *device, const char *driver);
void sysfs_pci_unbound(const struct pci_device *device);
void sysfs_pci_driver_added(const char *driver);
void sysfs_pci_driver_removed(const char *driver);
void sysfs_publish_sound(unsigned card, int playback, int capture, const char *id,
                         uint32_t location);
void sysfs_remove_sound(unsigned card, uint32_t location);
void sysfs_publish_sound_timer(void);
// card0, and renderD128 when there is 3D, for whichever driver now backs DRM.
void sysfs_publish_drm(void);
void sysfs_remove_drm(void);

// An ACPI device as Linux shows it: /sys/bus/acpi/devices/NAME with its modalias, so
// that modprobe finds the module for it. NAME is "PNP0C0C:00"; PATH is its namespace path.
void sysfs_publish_acpi_device(const char *name, const char *modalias, const char *path);

typedef int (*thermal_zone_reader)(unsigned index, int32_t *millicelsius);
void sysfs_publish_thermal_zone(unsigned index, const char *type, thermal_zone_reader reader);

#endif
