#ifndef TUNIX_SYSFS_H
#define TUNIX_SYSFS_H

struct module;

void sysfs_init(void);
void sysfs_module_added(struct module *module);
void sysfs_module_removed(const char *name);

#endif
