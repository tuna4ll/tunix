/*
 * fb-shot -- write the current contents of the screen to a PPM file.
 *
 * The framebuffer is readable regardless of who owns it for drawing, so this
 * captures whatever is actually on the display: the text console, a program
 * that took over /dev/fb0, or a compositor presenting through DRM. That last
 * one is why it exists -- on a headless build there is no other way to find
 * out whether weston's pixels reached the scanout, and "the log says it
 * enabled an output" is not the same claim.
 *
 *   fb-shot [path]      default /shot.ppm
 *
 * The output is binary PPM (P6): three bytes per pixel, no compression, no
 * dependencies. The framebuffer's channel masks are undone on the way out so
 * the file is plain RGB whatever the hardware layout is.
 */

#include <stddef.h>
#include <stdint.h>
#include "tunix_libc.h"
#include <tunix/framebuffer.h>

#define ROW_LIMIT 8192U

static void put(const char *text) {
    size_t length = 0;
    while (text[length]) length++;
    (void)t_write(1, text, length);
}

static void put_number(uint32_t value) {
    char digits[12];
    int count = 0;
    if (!value) digits[count++] = '0';
    while (value && count < (int)sizeof(digits)) {
        digits[count++] = (char)('0' + value % 10U);
        value /= 10U;
    }
    char out[12];
    int length = 0;
    while (count-- > 0) out[length++] = digits[count];
    (void)t_write(1, out, (size_t)length);
}

/* Append a decimal number and a trailing character to the PPM header. */
static size_t append_number(char *out, size_t used, uint32_t value, char tail) {
    char digits[12];
    int count = 0;
    if (!value) digits[count++] = '0';
    while (value && count < (int)sizeof(digits)) {
        digits[count++] = (char)('0' + value % 10U);
        value /= 10U;
    }
    while (count-- > 0) out[used++] = digits[count];
    out[used++] = tail;
    return used;
}

static uint32_t mask_of(uint32_t size) {
    if (!size) return 0;
    if (size >= 32U) return 0xFFFFFFFFU;
    return (1U << size) - 1U;
}

/* Undo one channel of the hardware's packing back to an 8-bit value. */
static uint8_t unpack(uint32_t pixel, uint32_t position, uint32_t size) {
    uint32_t mask = mask_of(size);
    if (!mask) return 0;
    uint32_t value = (pixel >> position) & mask;
    return (uint8_t)((value * 255U + mask / 2U) / mask);
}

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "/shot.ppm";

    int fb = t_open("/dev/fb0", T_O_RDONLY, 0);
    if (fb < 0) {
        put("fb-shot: cannot open /dev/fb0\n");
        return 1;
    }

    struct tunix_fb_info info;
    if (t_ioctl(fb, TUNIX_FBIO_GET_INFO, &info) != 0) {
        put("fb-shot: cannot read framebuffer info\n");
        t_close(fb);
        return 1;
    }
    if (info.bits_per_pixel != 32U || info.pitch > ROW_LIMIT * 4U) {
        put("fb-shot: unsupported framebuffer format\n");
        t_close(fb);
        return 1;
    }

    int out = t_open(path, T_O_WRONLY | T_O_CREAT | T_O_TRUNC, 0644);
    if (out < 0) {
        put("fb-shot: cannot create ");
        put(path);
        put("\n");
        t_close(fb);
        return 1;
    }

    char header[64];
    size_t used = 0;
    header[used++] = 'P';
    header[used++] = '6';
    header[used++] = '\n';
    used = append_number(header, used, info.width, ' ');
    used = append_number(header, used, info.height, '\n');
    used = append_number(header, used, 255U, '\n');
    (void)t_write(out, header, used);

    /* One scanline at a time: the whole framebuffer is several megabytes and
       there is no reason to hold it all at once. */
    static uint32_t source[ROW_LIMIT];
    static uint8_t line[ROW_LIMIT * 3U];

    for (uint32_t y = 0; y < info.height; y++) {
        size_t remaining = info.pitch;
        uint8_t *cursor = (uint8_t *)source;
        while (remaining) {
            long got = t_read(fb, cursor, remaining);
            if (got <= 0) {
                put("fb-shot: short read at row ");
                put_number(y);
                put("\n");
                t_close(out);
                t_close(fb);
                return 1;
            }
            cursor += got;
            remaining -= (size_t)got;
        }

        for (uint32_t x = 0; x < info.width; x++) {
            uint32_t pixel = source[x];
            line[x * 3U + 0U] = unpack(pixel, info.red_field_position, info.red_mask_size);
            line[x * 3U + 1U] = unpack(pixel, info.green_field_position, info.green_mask_size);
            line[x * 3U + 2U] = unpack(pixel, info.blue_field_position, info.blue_mask_size);
        }
        (void)t_write(out, line, info.width * 3U);
    }

    t_close(out);
    t_close(fb);

    put("fb-shot: wrote ");
    put_number(info.width);
    put("x");
    put_number(info.height);
    put(" to ");
    put(path);
    /* Who owned the display matters when reading the result: a black image
       means something different if the console still had it. */
    put(info.mode == TUNIX_FB_MODE_GRAPHICS ? " (graphics mode)\n"
                                            : " (console mode)\n");
    return 0;
}
