#include <stddef.h>
#include <stdint.h>
#include "include/framebuffer.h"
#include "include/vmm.h"

#define FRAMEBUFFER_VIRTUAL_BASE 0xFFFFFFFFD0000000ULL

struct framebuffer_state {
    volatile uint8_t *base;
    uint64_t physical_address;
    uint64_t byte_length;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint8_t red_size;
    uint8_t red_position;
    uint8_t green_size;
    uint8_t green_position;
    uint8_t blue_size;
    uint8_t blue_position;
    const uint8_t *font;
    uint16_t font_width;
    uint16_t font_height;
    int ready;
};

static struct framebuffer_state framebuffer;

static uint32_t scale_component(uint8_t value, uint8_t mask_size) {
    if (!mask_size) return 0;
    uint32_t maximum = mask_size >= 31U ? 0x7FFFFFFFU : ((1U << mask_size) - 1U);
    return ((uint32_t)value * maximum + 127U) / 255U;
}

int framebuffer_init(const struct boot_framebuffer_info *boot_info) {
    if (!boot_info || boot_info->magic != TUNIX_BOOT_FB_MAGIC ||
        boot_info->version != TUNIX_BOOT_FB_VERSION ||
        boot_info->size < sizeof(*boot_info)) return -1;
    if (boot_info->bits_per_pixel != 32U || !boot_info->physical_address ||
        boot_info->width < 640U || boot_info->height < 480U ||
        boot_info->width > TUNIX_FRAMEBUFFER_MAX_WIDTH ||
        boot_info->height > TUNIX_FRAMEBUFFER_MAX_HEIGHT ||
        boot_info->pitch < (uint32_t)boot_info->width * 4U) return -1;
    if (!boot_info->red_mask_size || !boot_info->green_mask_size ||
        !boot_info->blue_mask_size) return -1;

    uint64_t physical_page = boot_info->physical_address & ~0xFFFULL;
    uint64_t page_offset = boot_info->physical_address & 0xFFFULL;
    uint64_t byte_count = (uint64_t)boot_info->pitch * boot_info->height + page_offset;
    uint64_t mapped_size = (byte_count + 0xFFFULL) & ~0xFFFULL;
    for (uint64_t offset = 0; offset < mapped_size; offset += 4096ULL) {
        int result = vmm_map_page_in(vmm_kernel_cr3(), FRAMEBUFFER_VIRTUAL_BASE + offset,
                                     physical_page + offset, PAGE_WRITE);
        if (result != 0 && result != -2) return -1;
    }

    framebuffer.base = (volatile uint8_t *)(FRAMEBUFFER_VIRTUAL_BASE + page_offset);
    framebuffer.physical_address = boot_info->physical_address;
    framebuffer.byte_length = (uint64_t)boot_info->pitch * boot_info->height;
    framebuffer.width = boot_info->width;
    framebuffer.height = boot_info->height;
    framebuffer.pitch = boot_info->pitch;
    framebuffer.red_size = boot_info->red_mask_size;
    framebuffer.red_position = boot_info->red_field_position;
    framebuffer.green_size = boot_info->green_mask_size;
    framebuffer.green_position = boot_info->green_field_position;
    framebuffer.blue_size = boot_info->blue_mask_size;
    framebuffer.blue_position = boot_info->blue_field_position;
    framebuffer.font = (const uint8_t *)vmm_phys_to_virt(boot_info->font_physical_address);
    framebuffer.font_width = boot_info->font_width;
    framebuffer.font_height = boot_info->font_height;
    framebuffer.ready = 1;
    framebuffer_fill_rgb(0x060B12U);
    return 0;
}

int framebuffer_available(void) { return framebuffer.ready; }
uint32_t framebuffer_width(void) { return framebuffer.width; }
uint32_t framebuffer_height(void) { return framebuffer.height; }
uint32_t framebuffer_pitch(void) { return framebuffer.pitch; }
uint32_t framebuffer_bits_per_pixel(void) { return framebuffer.ready ? 32U : 0U; }
uint64_t framebuffer_physical_address(void) { return framebuffer.physical_address; }
uint64_t framebuffer_byte_length(void) { return framebuffer.byte_length; }
uint8_t framebuffer_red_size(void) { return framebuffer.red_size; }
uint8_t framebuffer_red_position(void) { return framebuffer.red_position; }
uint8_t framebuffer_green_size(void) { return framebuffer.green_size; }
uint8_t framebuffer_green_position(void) { return framebuffer.green_position; }
uint8_t framebuffer_blue_size(void) { return framebuffer.blue_size; }
uint8_t framebuffer_blue_position(void) { return framebuffer.blue_position; }
const uint8_t *framebuffer_font(void) { return framebuffer.font; }
uint32_t framebuffer_font_width(void) { return framebuffer.font_width; }
uint32_t framebuffer_font_height(void) { return framebuffer.font_height; }

uint32_t framebuffer_pack_rgb(uint32_t rgb) {
    uint8_t red = (uint8_t)(rgb >> 16);
    uint8_t green = (uint8_t)(rgb >> 8);
    uint8_t blue = (uint8_t)rgb;
    if (framebuffer.red_size == 8U && framebuffer.green_size == 8U &&
        framebuffer.blue_size == 8U) {
        return ((uint32_t)red << framebuffer.red_position) |
               ((uint32_t)green << framebuffer.green_position) |
               ((uint32_t)blue << framebuffer.blue_position);
    }
    return (scale_component(red, framebuffer.red_size) << framebuffer.red_position) |
           (scale_component(green, framebuffer.green_size) << framebuffer.green_position) |
           (scale_component(blue, framebuffer.blue_size) << framebuffer.blue_position);
}

void framebuffer_put_native(uint32_t x, uint32_t y, uint32_t native_pixel) {
    if (!framebuffer.ready || x >= framebuffer.width || y >= framebuffer.height) return;
    volatile uint32_t *pixel = (volatile uint32_t *)(framebuffer.base +
                               (uint64_t)y * framebuffer.pitch + (uint64_t)x * 4U);
    *pixel = native_pixel;
}

void framebuffer_put_rgb(uint32_t x, uint32_t y, uint32_t rgb) {
    framebuffer_put_native(x, y, framebuffer_pack_rgb(rgb));
}

void framebuffer_copy_rect(uint32_t destination_x, uint32_t destination_y,
                           uint32_t source_x, uint32_t source_y,
                           uint32_t width, uint32_t height) {
    if (!framebuffer.ready || !width || !height ||
        destination_x >= framebuffer.width || source_x >= framebuffer.width ||
        destination_y >= framebuffer.height || source_y >= framebuffer.height)
        return;

    uint32_t source_width = framebuffer.width - source_x;
    uint32_t destination_width = framebuffer.width - destination_x;
    uint32_t source_height = framebuffer.height - source_y;
    uint32_t destination_height = framebuffer.height - destination_y;
    if (width > source_width) width = source_width;
    if (width > destination_width) width = destination_width;
    if (height > source_height) height = source_height;
    if (height > destination_height) height = destination_height;
    if (!width || !height ||
        (destination_x == source_x && destination_y == source_y))
        return;

    int copy_bottom_up = destination_y > source_y &&
                         destination_y < source_y + height;
    for (uint32_t row_index = 0; row_index < height; row_index++) {
        uint32_t row = copy_bottom_up ? height - 1U - row_index : row_index;
        volatile uint32_t *source = (volatile uint32_t *)(framebuffer.base +
                                    (uint64_t)(source_y + row) * framebuffer.pitch) + source_x;
        volatile uint32_t *destination = (volatile uint32_t *)(framebuffer.base +
                                         (uint64_t)(destination_y + row) * framebuffer.pitch) +
                                         destination_x;

        if (destination_y + row == source_y + row &&
            destination_x > source_x && destination_x < source_x + width) {
            for (uint32_t column = width; column > 0; column--)
                destination[column - 1U] = source[column - 1U];
        } else {
            for (uint32_t column = 0; column < width; column++)
                destination[column] = source[column];
        }
    }
}

void framebuffer_fill_rgb(uint32_t rgb) {
    if (!framebuffer.ready) return;
    uint32_t native = framebuffer_pack_rgb(rgb);
    for (uint32_t y = 0; y < framebuffer.height; y++) {
        volatile uint32_t *row = (volatile uint32_t *)(framebuffer.base +
                                 (uint64_t)y * framebuffer.pitch);
        for (uint32_t x = 0; x < framebuffer.width; x++) row[x] = native;
    }
}
