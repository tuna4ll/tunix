#include <stddef.h>
#include <stdint.h>
#include "include/file.h"
#include "include/framebuffer.h"
#include "include/terminal.h"
#include "include/usercopy.h"
#include "include/vmm.h"
#include "include/vt.h"

#include "../include/tunix/framebuffer.h"

extern void kprintf(const char *fmt, ...);

/* Must sit ABOVE the kernel heap window, or the heap collides with it as it
   grows and stops early. The heap is [HEAP_START, HEAP_START+HEAP_MAX_SIZE) =
   [0xFFFFFFFFC0000000, 0xFFFFFFFFF0000000) (768 MiB); this is the first address
   past it, leaving 256 MiB up to the top of the address space for the display. */
#define FRAMEBUFFER_VIRTUAL_BASE 0xFFFFFFFFF0000000ULL

#define EACCES 13
#define EBUSY 16
#define EFAULT 14
#define EINVAL 22
#define ENODEV 19
#define ENOTTY 25
#define EPERM 1

struct framebuffer_state {
    volatile uint8_t *base;
    uint64_t physical_address;
    uint64_t physical_page;
    uint64_t byte_length;
    uint64_t mapping_size;
    uint64_t memory_offset;
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
    /* Whoever currently owns the display. Usually an open /dev/fb0 description,
       but the DRM device claims it with a token of its own (see drm.c), so this
       is deliberately untyped: the pointer is only ever compared. */
    const void *graphics_owner;
    /* The owner is on a virtual terminal that is not the one in front. It keeps
       its claim -- it is still running and still owns its buffers -- but it
       does not reach the screen, and the console may draw again. */
    int graphics_suspended;
    int ready;
};

static struct framebuffer_state framebuffer;

static uint64_t interrupt_save(void) {
    uint64_t flags;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) : : "memory");
    return flags;
}

static void interrupt_restore(uint64_t flags) {
    __asm__ volatile("pushq %0; popfq" : : "r"(flags) : "memory", "cc");
}

static uint64_t align_up_page(uint64_t value) {
    return (value + 4095ULL) & ~4095ULL;
}

static uint32_t scale_component(uint8_t value, uint8_t mask_size) {
    if (!mask_size) return 0;
    uint32_t maximum = mask_size >= 31U ? 0x7FFFFFFFU : ((1U << mask_size) - 1U);
    return ((uint32_t)value * maximum + 127U) / 255U;
}

static int file_is_writable(const struct file *file) {
    return file && (file->flags & 3U) != 0U;
}

static int framebuffer_owner_is(const void *owner) {
    uint64_t interrupt_flags = interrupt_save();
    int owned = framebuffer.graphics_owner == owner;
    interrupt_restore(interrupt_flags);
    return owned;
}

int framebuffer_claim_graphics(const void *owner) {
    if (!framebuffer.ready) return -ENODEV;
    if (!owner) return -EINVAL;

    uint64_t interrupt_flags = interrupt_save();
    if (framebuffer.graphics_owner && framebuffer.graphics_owner != owner) {
        interrupt_restore(interrupt_flags);
        return -EBUSY;
    }
    int first_claim = framebuffer.graphics_owner == NULL;
    framebuffer.graphics_owner = owner;
    interrupt_restore(interrupt_flags);
    /* A new owner belongs to the terminal that is in front of the user now;
       that is what decides where the display goes back to on a switch. */
    if (first_claim) vt_display_claimed();
    return 0;
}

int framebuffer_release_graphics(const void *owner, int fail_if_not_owner) {
    if (!framebuffer.ready) return -ENODEV;

    uint64_t interrupt_flags = interrupt_save();
    if (framebuffer.graphics_owner != owner) {
        int no_owner = framebuffer.graphics_owner == NULL;
        interrupt_restore(interrupt_flags);
        if (no_owner) return 0;
        return fail_if_not_owner ? -EPERM : 0;
    }
    framebuffer.graphics_owner = NULL;
    framebuffer.graphics_suspended = 0;
    interrupt_restore(interrupt_flags);

    vt_display_released();
    /* Whatever was on screen belonged to the owner that just left, so the
       console has to paint itself back. */
    terminal_redraw();
    return 0;
}

void framebuffer_suspend_graphics(void) {
    uint64_t interrupt_flags = interrupt_save();
    if (framebuffer.graphics_owner) framebuffer.graphics_suspended = 1;
    interrupt_restore(interrupt_flags);
}

void framebuffer_resume_graphics(void) {
    uint64_t interrupt_flags = interrupt_save();
    framebuffer.graphics_suspended = 0;
    interrupt_restore(interrupt_flags);
}

int framebuffer_graphics_foreground(const void *owner) {
    uint64_t interrupt_flags = interrupt_save();
    int foreground = framebuffer.graphics_owner == owner &&
                     !framebuffer.graphics_suspended;
    interrupt_restore(interrupt_flags);
    return foreground;
}

/* /dev/fb0 owns by open file description, and only a writable one may draw. */
static int framebuffer_acquire(struct file *file) {
    if (!framebuffer.ready) return -ENODEV;
    if (!file_is_writable(file)) return -EACCES;
    return framebuffer_claim_graphics(file);
}

static int framebuffer_release(struct file *file, int fail_if_not_owner) {
    return framebuffer_release_graphics(file, fail_if_not_owner);
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
    uint64_t framebuffer_bytes = (uint64_t)boot_info->pitch * boot_info->height;
    if (framebuffer_bytes > UINT64_MAX - page_offset) return -1;
    uint64_t mapped_size = align_up_page(framebuffer_bytes + page_offset);
    /* Write-combining where the processor can do it, uncached where it cannot.
       Never write-back: this is a device's memory, and the writes have to
       reach it rather than sit in a cache line waiting for an eviction. The
       distinction costs nothing under emulation, where the framebuffer is
       ordinary RAM, and is the difference between a usable desktop and an
       unusable one on a real card. */
    uint64_t cache_flags = vmm_write_combining_available() ? PAGE_WRITE_COMBINING
                                                          : PAGE_UNCACHED;
    for (uint64_t offset = 0; offset < mapped_size; offset += 4096ULL) {
        int result = vmm_map_page_in(vmm_kernel_cr3(), FRAMEBUFFER_VIRTUAL_BASE + offset,
                                     physical_page + offset,
                                     PAGE_WRITE | PAGE_DEVICE | cache_flags);
        if (result != 0 && result != -2) return -1;
    }

    kprintf("FB: %ux%u mapped %s\n", (unsigned)boot_info->width,
            (unsigned)boot_info->height,
            vmm_write_combining_available() ? "write-combining" : "uncached");

    framebuffer.base = (volatile uint8_t *)(FRAMEBUFFER_VIRTUAL_BASE + page_offset);
    framebuffer.physical_address = boot_info->physical_address;
    framebuffer.physical_page = physical_page;
    framebuffer.byte_length = framebuffer_bytes;
    framebuffer.mapping_size = mapped_size;
    framebuffer.memory_offset = page_offset;
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
    framebuffer.graphics_owner = NULL;
    framebuffer.ready = 1;
    framebuffer_fill_rgb(0x000000U);
    return 0;
}

int framebuffer_available(void) { return framebuffer.ready; }
int framebuffer_console_active(void) {
    return framebuffer.ready &&
           (framebuffer.graphics_owner == NULL || framebuffer.graphics_suspended);
}
uint32_t framebuffer_width(void) { return framebuffer.width; }
uint32_t framebuffer_height(void) { return framebuffer.height; }
uint32_t framebuffer_pitch(void) { return framebuffer.pitch; }
uint32_t framebuffer_bits_per_pixel(void) { return framebuffer.ready ? 32U : 0U; }
uint64_t framebuffer_physical_address(void) { return framebuffer.physical_address; }
uint64_t framebuffer_byte_length(void) { return framebuffer.byte_length; }
uint64_t framebuffer_mapping_size(void) { return framebuffer.mapping_size; }
uint64_t framebuffer_memory_offset(void) { return framebuffer.memory_offset; }
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
    if (!framebuffer_console_active() || x >= framebuffer.width || y >= framebuffer.height)
        return;
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
    if (!framebuffer_console_active() || !width || !height ||
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
    if (!framebuffer_console_active()) return;
    uint32_t native = framebuffer_pack_rgb(rgb);
    for (uint32_t y = 0; y < framebuffer.height; y++) {
        volatile uint32_t *row = (volatile uint32_t *)(framebuffer.base +
                                 (uint64_t)y * framebuffer.pitch);
        for (uint32_t x = 0; x < framebuffer.width; x++) row[x] = native;
    }
}

int64_t framebuffer_file_read(struct file *file, size_t size, void *buffer) {
    if (!framebuffer.ready || !file || !buffer) return -EINVAL;
    if (file->offset >= framebuffer.byte_length) return 0;
    uint64_t available = framebuffer.byte_length - file->offset;
    if ((uint64_t)size > available) size = (size_t)available;

    uint8_t *destination = (uint8_t *)buffer;
    const volatile uint8_t *source = framebuffer.base + file->offset;
    for (size_t index = 0; index < size; index++) destination[index] = source[index];
    file->offset += size;
    return (int64_t)size;
}

int64_t framebuffer_file_write(struct file *file, size_t size, const void *buffer) {
    if (!framebuffer.ready || !file || !buffer) return -EINVAL;
    if (!framebuffer_owner_is(file)) return -EPERM;
    if (file->offset >= framebuffer.byte_length) return 0;
    uint64_t available = framebuffer.byte_length - file->offset;
    if ((uint64_t)size > available) size = (size_t)available;

    const uint8_t *source = (const uint8_t *)buffer;
    volatile uint8_t *destination = framebuffer.base + file->offset;
    for (size_t index = 0; index < size; index++) destination[index] = source[index];
    file->offset += size;
    __sync_synchronize();
    return (int64_t)size;
}

static void framebuffer_fill_info(struct tunix_fb_info *info) {
    info->abi_version = TUNIX_FB_ABI_VERSION;
    info->width = framebuffer.width;
    info->height = framebuffer.height;
    info->pitch = framebuffer.pitch;
    info->bits_per_pixel = 32U;
    info->pixel_format = TUNIX_FB_PIXEL_FORMAT_BITMASK;
    info->red_mask_size = framebuffer.red_size;
    info->red_field_position = framebuffer.red_position;
    info->green_mask_size = framebuffer.green_size;
    info->green_field_position = framebuffer.green_position;
    info->blue_mask_size = framebuffer.blue_size;
    info->blue_field_position = framebuffer.blue_position;
    info->mode = framebuffer.graphics_owner ? TUNIX_FB_MODE_GRAPHICS :
                                              TUNIX_FB_MODE_CONSOLE;
    info->flags = TUNIX_FB_FLAG_LINEAR | TUNIX_FB_FLAG_DIRECT_MMAP;
    info->framebuffer_size = framebuffer.byte_length;
    info->mapping_size = framebuffer.mapping_size;
    info->memory_offset = framebuffer.memory_offset;
}

int64_t framebuffer_file_ioctl(struct file *file, unsigned long request,
                               uint64_t user_argument) {
    if (!framebuffer.ready || !file) return -ENODEV;

    if (request == TUNIX_FBIO_GET_INFO) {
        if (!user_argument) return -EFAULT;
        struct tunix_fb_info info;
        framebuffer_fill_info(&info);
        return copy_to_user(user_argument, &info, sizeof(info)) == 0 ? 0 : -EFAULT;
    }

    if (request == TUNIX_FBIO_GET_MODE) {
        if (!user_argument) return -EFAULT;
        uint32_t mode = framebuffer.graphics_owner ? TUNIX_FB_MODE_GRAPHICS :
                                                     TUNIX_FB_MODE_CONSOLE;
        return copy_to_user(user_argument, &mode, sizeof(mode)) == 0 ? 0 : -EFAULT;
    }

    if (request == TUNIX_FBIO_SET_MODE) {
        if (!user_argument) return -EFAULT;
        uint32_t mode;
        if (copy_from_user(&mode, user_argument, sizeof(mode)) != 0) return -EFAULT;
        if (mode == TUNIX_FB_MODE_GRAPHICS) return framebuffer_acquire(file);
        if (mode == TUNIX_FB_MODE_CONSOLE) return framebuffer_release(file, 1);
        return -EINVAL;
    }

    if (request == TUNIX_FBIO_FLUSH) {
        if (!framebuffer_owner_is(file)) return -EPERM;
        if (user_argument) {
            struct tunix_fb_rect rectangle;
            if (copy_from_user(&rectangle, user_argument, sizeof(rectangle)) != 0)
                return -EFAULT;
            if (!rectangle.width || !rectangle.height ||
                rectangle.x >= framebuffer.width || rectangle.y >= framebuffer.height ||
                rectangle.width > framebuffer.width - rectangle.x ||
                rectangle.height > framebuffer.height - rectangle.y)
                return -EINVAL;
        }
        __sync_synchronize();
        return 0;
    }

    return -ENOTTY;
}

void framebuffer_file_close(struct file *file) {
    if (file) (void)framebuffer_release(file, 0);
}

/*
 * The scanout as the kernel sees it, for drivers that composite in software.
 * The DRM device uses this to present a dumb buffer: there is no CRTC to point
 * at a different address, so presenting means copying into the one real
 * framebuffer the bootloader gave us.
 */
uint8_t *framebuffer_scanout(void) {
    if (!framebuffer.ready || !framebuffer.base) return NULL;
    return (uint8_t *)framebuffer.base + framebuffer.memory_offset;
}

/* The mapping is write-through to the display, so presenting is only a barrier;
   this is the same thing TUNIX_FBIO_FLUSH does. */
void framebuffer_present(void) {
    __sync_synchronize();
}

int64_t framebuffer_device_mmap(struct vfs_node *node, struct file *file,
                                uint64_t cr3, uint64_t virtual_address,
                                uint64_t length, uint64_t offset,
                                uint64_t page_flags) {
    (void)node;
    if (!framebuffer.ready || !file || !length) return -EINVAL;
    if (!framebuffer_owner_is(file)) return -EPERM;
    if ((offset & 0xFFFULL) || (length & 0xFFFULL)) return -EINVAL;
    if (offset >= framebuffer.mapping_size ||
        length > framebuffer.mapping_size - offset) return -EINVAL;

    uint64_t mapped = 0;
    /* The same caching decision as the kernel's own mapping, and it matters
       more here: this is the mapping Xorg and weston draw a whole screen
       through, a word at a time. */
    uint64_t flags = page_flags | PAGE_USER | PAGE_DEVICE | PAGE_PRESENT | PAGE_NX |
                     (vmm_write_combining_available() ? PAGE_WRITE_COMBINING
                                                      : PAGE_UNCACHED);
    for (; mapped < length; mapped += 4096ULL) {
        int status = vmm_map_page_in(cr3, virtual_address + mapped,
                                     framebuffer.physical_page + offset + mapped,
                                     flags);
        if (status != 0) {
            while (mapped) {
                mapped -= 4096ULL;
                (void)vmm_unmap_page_in(cr3, virtual_address + mapped);
            }
            return -EINVAL;
        }
    }
    return 0;
}
