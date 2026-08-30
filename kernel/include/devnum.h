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

/* The virtual terminals, numbered as Linux numbers them: major 4 is tty0 (the
   active one) and ttyN after it, and major 5 holds the two indirect nodes --
   /dev/tty, the caller's own controlling terminal, and /dev/console. */
#define DEV_MAJOR_TTY 4
#define DEV_MAJOR_TTYAUX 5
#define DEV_MINOR_TTYAUX_CURRENT 0
#define DEV_MINOR_TTYAUX_CONSOLE 1
#define DEV_MAJOR_INPUT 13
/* event0.. are minor 64 and up on Linux. */
#define DEV_MINOR_INPUT_EVENT_BASE 64
#define DEV_MAJOR_DRM 226
#define DEV_MINOR_DRM_CARD0 0
/* Render nodes start at 128 on Linux. A card and its render node are the same
   hardware reached two ways: the card carries the display, the render node
   carries only the drawing, and mesa wants the second one. */
#define DEV_MINOR_DRM_RENDER0 128
#define DEV_MAJOR_SOUND 116
/* Card 0 takes minors 0..31: the control node at the bottom, playback PCMs
   from 16 and capture ones from 24, as ALSA numbers them. */
#define DEV_MINOR_SOUND_CONTROL 0
#define DEV_MINOR_SOUND_PCM_PLAYBACK 16

/*
 * The groups a device node belongs to. They are how a normal user reaches the
 * hardware at all: the desktop needs the display and the input devices, and
 * without these it would need to be root to have them. /etc/group carries the
 * same numbers.
 */
#define DEV_GROUP_TTY 5
#define DEV_GROUP_DISK 6
#define DEV_GROUP_AUDIO 29
#define DEV_GROUP_VIDEO 44
#define DEV_GROUP_INPUT 45

#endif
