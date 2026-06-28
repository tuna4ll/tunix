#ifndef TUNIX_KEYMAP_ABI_H
#define TUNIX_KEYMAP_ABI_H

#include <stdint.h>

#define TUNIX_KEYMAP_ABI_VERSION 1U
#define TUNIX_KEYMAP_KEYCODES 128U
#define TUNIX_KEYMAP_LEVELS 8U
#define TUNIX_KEYMAP_NAME_MAX 32U
#define TUNIX_KEYMAP_LETTER_BYTES (TUNIX_KEYMAP_KEYCODES / 8U)

#define TUNIX_KEYMAP_LEVEL_SHIFT 0x01U
#define TUNIX_KEYMAP_LEVEL_ALTGR 0x02U
#define TUNIX_KEYMAP_LEVEL_CTRL  0x04U

#define TUNIX_KEYSYM_NONE UINT32_C(0xffffffff)

/* Tunix console-keymap ioctls. The payload is struct tunix_keymap. */
#define TUNIX_KDGKBMAP 0x4B70UL
#define TUNIX_KDSKBMAP 0x4B71UL

struct tunix_keymap {
    uint32_t version;
    uint32_t flags;
    char name[TUNIX_KEYMAP_NAME_MAX];
    uint8_t letter_bitmap[TUNIX_KEYMAP_LETTER_BYTES];
    uint32_t symbols[TUNIX_KEYMAP_LEVELS][TUNIX_KEYMAP_KEYCODES];
};

#endif
