#ifndef TUNIX_SYSFS_H
#define TUNIX_SYSFS_H

struct module;
struct pci_device;

void sysfs_init(void);
void sysfs_module_added(struct module *module);
void sysfs_module_removed(const char *name);
void sysfs_pci_bound(const struct pci_device *device, const char *driver);
void sysfs_pci_unbound(const struct pci_device *device);
void sysfs_pci_driver_added(const char *driver);
void sysfs_pci_driver_removed(const char *driver);
void sysfs_publish_sound(void);
void sysfs_remove_sound(void);

#endif
