#ifndef TUNIX_BOOT_FRAMEBUFFER_H
#define TUNIX_BOOT_FRAMEBUFFER_H

#include <stddef.h>
#include <stdint.h>

#define TUNIX_BOOT_FB_MAGIC 0x30424654U
#define TUNIX_BOOT_FB_VERSION 1U
#define TUNIX_BIOS_FONT_PHYSICAL 0x00006000ULL

struct boot_framebuffer_info {
    uint32_t magic;
    uint16_t version;
    uint16_t size;
    uint64_t physical_address;
    uint32_t pitch;
    uint16_t width;
    uint16_t height;
    uint8_t bits_per_pixel;
    uint8_t red_mask_size;
    uint8_t red_field_position;
    uint8_t green_mask_size;
    uint8_t green_field_position;
    uint8_t blue_mask_size;
    uint8_t blue_field_position;
    uint8_t reserved_mask_size;
    uint8_t reserved_field_position;
    uint8_t padding[7];
    uint64_t font_physical_address;
    uint16_t font_width;
    uint16_t font_height;
} __attribute__((packed));

_Static_assert(sizeof(struct boot_framebuffer_info) == 52U,
               "boot framebuffer ABI size mismatch");
_Static_assert(offsetof(struct boot_framebuffer_info, font_physical_address) == 40U,
               "boot framebuffer ABI offset mismatch");

#endif
