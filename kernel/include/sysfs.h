#ifndef TUNIX_SYSFS_H
#define TUNIX_SYSFS_H

/* Builds the minimal /sys tree udev needs to enumerate devices; see sysfs.c. */
void sysfs_init(void);

#endif
