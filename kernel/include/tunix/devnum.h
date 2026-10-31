#ifndef TUNIX_DEVNUM_H
#define TUNIX_DEVNUM_H

#include <stdint.h>

#define DEV_MAJOR_TTY              4
#define DEV_MAJOR_TTYAUX           5
#define DEV_MINOR_TTYAUX_CURRENT   0
#define DEV_MINOR_TTYAUX_CONSOLE   1
#define DEV_MINOR_TTYAUX_PTMX      2
#define DEV_MAJOR_PTS              136
#define DEV_MAJOR_INPUT            13
#define DEV_MINOR_INPUT_EVENT_BASE 64
#define DEV_MAJOR_DRM              226
#define DEV_MINOR_DRM_CARD0        0
#define DEV_MINOR_DRM_RENDER0      128
#define DEV_MAJOR_SOUND            116

#define DEV_GROUP_TTY   5
#define DEV_GROUP_DISK  9
#define DEV_GROUP_AUDIO 12
#define DEV_GROUP_VIDEO 13
#define DEV_GROUP_INPUT 25

static inline uint32_t dev_encode(uint32_t major, uint32_t minor) {
    return (minor & 0xFFU) | ((major & 0xFFFU) << 8) | ((minor & ~0xFFU) << 12);
}

#endif
