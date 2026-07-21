#ifndef TUNIX_DEVNUM_H
#define TUNIX_DEVNUM_H

/*
 * Device numbers, Linux's.
 *
 * These are not free choices. Userspace matches on them: libinput fstat()s an
 * evdev node and asks udev for the device with that rdev, and udev finds it
 * through /sys/dev/char/<major>:<minor>. devfs stamps them onto the node and
 * sysfs publishes the same pair, so the two cannot drift apart.
 */

#define DEV_MAJOR_INPUT 13
/* event0.. are minor 64 and up on Linux. */
#define DEV_MINOR_INPUT_EVENT_BASE 64
#define DEV_MAJOR_DRM 226
#define DEV_MINOR_DRM_CARD0 0

#endif
