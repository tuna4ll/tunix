#ifndef TUNIX_FRAMEBUFFER_ABI_H
#define TUNIX_FRAMEBUFFER_ABI_H

#include <stdint.h>

#define TUNIX_FB_ABI_VERSION 1U

#define TUNIX_FB_PIXEL_FORMAT_BITMASK 1U

#define TUNIX_FB_MODE_CONSOLE  0U
#define TUNIX_FB_MODE_GRAPHICS 1U

#define TUNIX_FB_FLAG_LINEAR      (1U << 0)
#define TUNIX_FB_FLAG_DIRECT_MMAP (1U << 1)

#define TUNIX_FBIO_GET_INFO 0x54460001UL
#define TUNIX_FBIO_GET_MODE 0x54460002UL
#define TUNIX_FBIO_SET_MODE 0x54460003UL
#define TUNIX_FBIO_FLUSH    0x54460004UL

struct tunix_fb_info {
    uint32_t abi_version;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t bits_per_pixel;
    uint32_t pixel_format;
    uint32_t red_mask_size;
    uint32_t red_field_position;
    uint32_t green_mask_size;
    uint32_t green_field_position;
    uint32_t blue_mask_size;
    uint32_t blue_field_position;
    uint32_t mode;
    uint32_t flags;
    uint64_t framebuffer_size;
    uint64_t mapping_size;
    uint64_t memory_offset;
};

struct tunix_fb_rect {
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
};

_Static_assert(sizeof(struct tunix_fb_info) == 80U,
               "Tunix framebuffer info ABI must remain 80 bytes");
_Static_assert(sizeof(struct tunix_fb_rect) == 16U,
               "Tunix framebuffer rectangle ABI must remain 16 bytes");

#endif
