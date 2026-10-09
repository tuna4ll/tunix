#ifndef TUNIX_FRAMEBUFFER_H
#define TUNIX_FRAMEBUFFER_H

#include <stddef.h>
#include <stdint.h>
#include <tunix/boot_framebuffer.h>

struct file;
struct vfs_node;

int framebuffer_init(const struct boot_framebuffer_info *boot_info);
int framebuffer_available(void);
int framebuffer_console_active(void);
uint32_t framebuffer_width(void);
uint32_t framebuffer_height(void);
uint32_t framebuffer_pitch(void);
uint32_t framebuffer_bits_per_pixel(void);
uint64_t framebuffer_physical_address(void);
uint64_t framebuffer_byte_length(void);
uint64_t framebuffer_mapping_size(void);
uint64_t framebuffer_memory_offset(void);
uint8_t framebuffer_red_size(void);
uint8_t framebuffer_red_position(void);
uint8_t framebuffer_green_size(void);
uint8_t framebuffer_green_position(void);
uint8_t framebuffer_blue_size(void);
uint8_t framebuffer_blue_position(void);
uint32_t framebuffer_pack_rgb(uint32_t rgb);
void framebuffer_put_rgb(uint32_t x, uint32_t y, uint32_t rgb);
void framebuffer_put_native(uint32_t x, uint32_t y, uint32_t native_pixel);
void framebuffer_fill_rect_rgb(uint32_t x, uint32_t y, uint32_t width, uint32_t height,
                               uint32_t rgb);
void framebuffer_measure(unsigned rounds, uint64_t *read_rate, uint64_t *write_rate);
void framebuffer_fill_rgb(uint32_t rgb);

uint8_t *framebuffer_scanout(void);
void framebuffer_present(void);

int framebuffer_claim_graphics(const void *owner);
int framebuffer_release_graphics(const void *owner, int fail_if_not_owner);

void framebuffer_suspend_graphics(void);
void framebuffer_resume_graphics(void);

int framebuffer_graphics_foreground(const void *owner);
const uint8_t *framebuffer_font(void);
uint32_t framebuffer_font_width(void);
uint32_t framebuffer_font_height(void);

int64_t framebuffer_file_read(struct file *file, size_t size, void *buffer);
int64_t framebuffer_file_write(struct file *file, size_t size, const void *buffer);
int64_t framebuffer_file_ioctl(struct file *file, unsigned long request,
                               uint64_t user_argument);
void framebuffer_file_close(struct file *file);
int64_t framebuffer_device_mmap(struct vfs_node *node, struct file *file,
                                uint64_t cr3, uint64_t virtual_address,
                                uint64_t length, uint64_t offset,
                                uint64_t page_flags);

#endif
