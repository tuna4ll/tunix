#ifndef TUNIX_FRAMEBUFFER_H
#define TUNIX_FRAMEBUFFER_H

#include <stdint.h>
#include "boot_framebuffer.h"

#define TUNIX_FRAMEBUFFER_MAX_WIDTH 1920U
#define TUNIX_FRAMEBUFFER_MAX_HEIGHT 1080U

int framebuffer_init(const struct boot_framebuffer_info *boot_info);
int framebuffer_available(void);
uint32_t framebuffer_width(void);
uint32_t framebuffer_height(void);
uint32_t framebuffer_pitch(void);
uint32_t framebuffer_bits_per_pixel(void);
uint64_t framebuffer_physical_address(void);
uint64_t framebuffer_byte_length(void);
uint8_t framebuffer_red_size(void);
uint8_t framebuffer_red_position(void);
uint8_t framebuffer_green_size(void);
uint8_t framebuffer_green_position(void);
uint8_t framebuffer_blue_size(void);
uint8_t framebuffer_blue_position(void);
uint32_t framebuffer_pack_rgb(uint32_t rgb);
void framebuffer_put_rgb(uint32_t x, uint32_t y, uint32_t rgb);
void framebuffer_put_native(uint32_t x, uint32_t y, uint32_t native_pixel);
void framebuffer_fill_rgb(uint32_t rgb);
const uint8_t *framebuffer_font(void);
uint32_t framebuffer_font_width(void);
uint32_t framebuffer_font_height(void);

#endif
