#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <jpeglib.h>
#include <png.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifndef TUNIX_LIBJPEG_TURBO_VERSION
#define TUNIX_LIBJPEG_TURBO_VERSION "unknown"
#endif
#ifndef TUNIX_ZLIB_VERSION
#define TUNIX_ZLIB_VERSION "unknown"
#endif

#define WALLPAPER_MAGIC 0x4C415754U
#define WALLPAPER_VERSION 1U
#define WALLPAPER_RGB565 1U
#define WALLPAPER_HEADER_SIZE 24U

struct image_rgb {
    uint32_t width;
    uint32_t height;
    uint8_t *pixels;
};

struct jpeg_error_context {
    struct jpeg_error_mgr base;
    jmp_buf jump;
    char message[JMSG_LENGTH_MAX];
};

static void usage(const char *program) {
    fprintf(stderr,
            "usage: %s <input.png|input.jpg> <output.twl> [--width N] [--height N]\n"
            "       %s --self-test\n"
            "       %s --version\n",
            program, program, program);
}

static int checked_rgb_size(uint32_t width, uint32_t height, size_t *size_out) {
    if (width == 0U || height == 0U) return -1;
    size_t row = (size_t)width * 3U;
    if ((size_t)height > SIZE_MAX / row) return -1;
    *size_out = row * (size_t)height;
    return 0;
}

static int checked_wallpaper_size(uint32_t width, uint32_t height, size_t *size_out) {
    if (width == 0U || height == 0U) return -1;
    size_t row = (size_t)width * 2U;
    if ((size_t)height > SIZE_MAX / row) return -1;
    *size_out = row * (size_t)height;
    return 0;
}

static void image_release(struct image_rgb *image) {
    if (!image) return;
    free(image->pixels);
    image->pixels = NULL;
    image->width = 0U;
    image->height = 0U;
}

static int read_magic(const char *path, uint8_t magic[8], size_t *amount_out) {
    FILE *file = fopen(path, "rb");
    if (!file) {
        fprintf(stderr, "wallpaper: cannot open %s: %s\n", path, strerror(errno));
        return -1;
    }
    size_t amount = fread(magic, 1U, 8U, file);
    if (ferror(file)) {
        fprintf(stderr, "wallpaper: cannot read %s\n", path);
        fclose(file);
        return -1;
    }
    fclose(file);
    *amount_out = amount;
    return 0;
}

static int is_png_magic(const uint8_t magic[8], size_t amount) {
    static const uint8_t signature[8] = {137U, 80U, 78U, 71U, 13U, 10U, 26U, 10U};
    return amount >= sizeof(signature) && memcmp(magic, signature, sizeof(signature)) == 0;
}

static int is_jpeg_magic(const uint8_t magic[8], size_t amount) {
    return amount >= 3U && magic[0] == 0xFFU && magic[1] == 0xD8U && magic[2] == 0xFFU;
}

static int decode_png_file(const char *path, struct image_rgb *output) {
    png_image image;
    memset(&image, 0, sizeof(image));
    image.version = PNG_IMAGE_VERSION;

    if (!png_image_begin_read_from_file(&image, path)) {
        fprintf(stderr, "wallpaper: libpng could not read %s: %s\n", path, image.message);
        return -1;
    }
    if (image.width > UINT32_MAX || image.height > UINT32_MAX) {
        fprintf(stderr, "wallpaper: PNG dimensions are too large\n");
        png_image_free(&image);
        return -1;
    }

    image.format = PNG_FORMAT_RGB;
    size_t size = 0U;
    if (checked_rgb_size((uint32_t)image.width, (uint32_t)image.height, &size) != 0 ||
        size != (size_t)PNG_IMAGE_SIZE(image)) {
        fprintf(stderr, "wallpaper: PNG allocation size overflow\n");
        png_image_free(&image);
        return -1;
    }

    uint8_t *pixels = malloc(size);
    if (!pixels) {
        fprintf(stderr, "wallpaper: out of memory while decoding PNG\n");
        png_image_free(&image);
        return -1;
    }
    if (!png_image_finish_read(&image, NULL, pixels, 0, NULL)) {
        fprintf(stderr, "wallpaper: libpng decode failed: %s\n", image.message);
        free(pixels);
        png_image_free(&image);
        return -1;
    }

    output->width = (uint32_t)image.width;
    output->height = (uint32_t)image.height;
    output->pixels = pixels;
    png_image_free(&image);
    return 0;
}

static void jpeg_error_exit(j_common_ptr common) {
    struct jpeg_error_context *context = (struct jpeg_error_context *)common->err;
    (*common->err->format_message)(common, context->message);
    longjmp(context->jump, 1);
}

static int decode_jpeg_stream(struct jpeg_decompress_struct *decoder,
                              struct image_rgb *output) {
    if (jpeg_read_header(decoder, TRUE) != JPEG_HEADER_OK) return -1;
    decoder->out_color_space = JCS_RGB;
    if (!jpeg_start_decompress(decoder)) return -1;
    if (decoder->output_components != 3U || decoder->output_width > UINT32_MAX ||
        decoder->output_height > UINT32_MAX) {
        jpeg_abort_decompress(decoder);
        return -1;
    }

    uint32_t width = (uint32_t)decoder->output_width;
    uint32_t height = (uint32_t)decoder->output_height;
    size_t size = 0U;
    if (checked_rgb_size(width, height, &size) != 0) {
        jpeg_abort_decompress(decoder);
        return -1;
    }

    uint8_t *pixels = malloc(size);
    if (!pixels) {
        jpeg_abort_decompress(decoder);
        return -1;
    }
    const size_t row_size = (size_t)width * 3U;
    while (decoder->output_scanline < decoder->output_height) {
        JSAMPROW row = pixels + (size_t)decoder->output_scanline * row_size;
        if (jpeg_read_scanlines(decoder, &row, 1U) != 1U) {
            free(pixels);
            jpeg_abort_decompress(decoder);
            return -1;
        }
    }
    if (!jpeg_finish_decompress(decoder)) {
        free(pixels);
        return -1;
    }

    output->width = width;
    output->height = height;
    output->pixels = pixels;
    return 0;
}

static int decode_jpeg_file(const char *path, struct image_rgb *output) {
    FILE *file = fopen(path, "rb");
    if (!file) {
        fprintf(stderr, "wallpaper: cannot open %s: %s\n", path, strerror(errno));
        return -1;
    }

    struct jpeg_decompress_struct decoder;
    struct jpeg_error_context error;
    memset(&decoder, 0, sizeof(decoder));
    memset(&error, 0, sizeof(error));
    decoder.err = jpeg_std_error(&error.base);
    error.base.error_exit = jpeg_error_exit;

    if (setjmp(error.jump)) {
        fprintf(stderr, "wallpaper: libjpeg-turbo decode failed: %s\n", error.message);
        jpeg_destroy_decompress(&decoder);
        fclose(file);
        return -1;
    }

    jpeg_create_decompress(&decoder);
    jpeg_stdio_src(&decoder, file);
    int result = decode_jpeg_stream(&decoder, output);
    if (result != 0) fprintf(stderr, "wallpaper: invalid or unsupported JPEG image\n");
    jpeg_destroy_decompress(&decoder);
    fclose(file);
    return result;
}

static int decode_image_file(const char *path, struct image_rgb *output, const char **codec_out) {
    uint8_t magic[8] = {0};
    size_t amount = 0U;
    if (read_magic(path, magic, &amount) != 0) return -1;

    if (is_png_magic(magic, amount)) {
        if (codec_out) *codec_out = "libpng";
        return decode_png_file(path, output);
    }
    if (is_jpeg_magic(magic, amount)) {
        if (codec_out) *codec_out = "libjpeg-turbo";
        return decode_jpeg_file(path, output);
    }

    fprintf(stderr, "wallpaper: %s is neither PNG nor JPEG\n", path);
    return -1;
}

static uint8_t interpolate_channel(uint8_t a, uint8_t b, uint32_t fraction) {
    uint32_t inverse = 65536U - fraction;
    return (uint8_t)(((uint32_t)a * inverse + (uint32_t)b * fraction + 32768U) >> 16U);
}

static uint8_t bilinear_channel(uint8_t c00, uint8_t c10, uint8_t c01, uint8_t c11,
                                uint32_t fraction_x, uint32_t fraction_y) {
    uint8_t top = interpolate_channel(c00, c10, fraction_x);
    uint8_t bottom = interpolate_channel(c01, c11, fraction_x);
    return interpolate_channel(top, bottom, fraction_y);
}

static void sample_bilinear(const struct image_rgb *source,
                            uint64_t source_x_fp, uint64_t source_y_fp,
                            uint8_t rgb[3]) {
    uint32_t x0 = (uint32_t)(source_x_fp >> 16U);
    uint32_t y0 = (uint32_t)(source_y_fp >> 16U);
    uint32_t fx = (uint32_t)(source_x_fp & 0xFFFFU);
    uint32_t fy = (uint32_t)(source_y_fp & 0xFFFFU);
    if (x0 >= source->width) x0 = source->width - 1U;
    if (y0 >= source->height) y0 = source->height - 1U;
    uint32_t x1 = x0 + 1U < source->width ? x0 + 1U : x0;
    uint32_t y1 = y0 + 1U < source->height ? y0 + 1U : y0;

    const uint8_t *p00 = source->pixels + ((size_t)y0 * source->width + x0) * 3U;
    const uint8_t *p10 = source->pixels + ((size_t)y0 * source->width + x1) * 3U;
    const uint8_t *p01 = source->pixels + ((size_t)y1 * source->width + x0) * 3U;
    const uint8_t *p11 = source->pixels + ((size_t)y1 * source->width + x1) * 3U;
    for (unsigned int channel = 0U; channel < 3U; channel++) {
        rgb[channel] = bilinear_channel(p00[channel], p10[channel], p01[channel], p11[channel],
                                        fx, fy);
    }
}

static uint16_t pack_rgb565(const uint8_t rgb[3]) {
    return (uint16_t)(((uint16_t)(rgb[0] >> 3U) << 11U) |
                      ((uint16_t)(rgb[1] >> 2U) << 5U) |
                      (uint16_t)(rgb[2] >> 3U));
}

static void store_le16(uint8_t *destination, uint16_t value) {
    destination[0] = (uint8_t)value;
    destination[1] = (uint8_t)(value >> 8U);
}

static void store_le32(uint8_t *destination, uint32_t value) {
    destination[0] = (uint8_t)value;
    destination[1] = (uint8_t)(value >> 8U);
    destination[2] = (uint8_t)(value >> 16U);
    destination[3] = (uint8_t)(value >> 24U);
}

static int write_all(FILE *file, const void *buffer, size_t size) {
    const uint8_t *bytes = buffer;
    while (size > 0U) {
        size_t written = fwrite(bytes, 1U, size, file);
        if (written == 0U) return -1;
        bytes += written;
        size -= written;
    }
    return 0;
}

static uint64_t scale_coordinate(uint32_t crop_origin, uint32_t crop_extent,
                                 uint32_t output_index, uint32_t output_extent) {
    uint64_t origin_fp = (uint64_t)crop_origin << 16U;
    uint64_t offset_fp = ((((uint64_t)output_index * 2U + 1U) * crop_extent) << 15U) /
                         output_extent;
    uint64_t coordinate = offset_fp >= 32768U
                              ? origin_fp + offset_fp - 32768U
                              : origin_fp;
    uint64_t last_fp = ((uint64_t)crop_origin + crop_extent - 1U) << 16U;
    return coordinate > last_fp ? last_fp : coordinate;
}

static int write_wallpaper(const char *path, const struct image_rgb *source,
                           uint32_t output_width, uint32_t output_height) {
    size_t payload_size = 0U;
    if (checked_wallpaper_size(output_width, output_height, &payload_size) != 0 ||
        payload_size > UINT32_MAX) {
        fprintf(stderr, "wallpaper: requested output is too large\n");
        return -1;
    }

    uint32_t crop_x = 0U;
    uint32_t crop_y = 0U;
    uint32_t crop_width = source->width;
    uint32_t crop_height = source->height;
    uint64_t source_ratio = (uint64_t)source->width * output_height;
    uint64_t target_ratio = (uint64_t)source->height * output_width;
    if (source_ratio > target_ratio) {
        crop_width = (uint32_t)(((uint64_t)source->height * output_width) / output_height);
        if (crop_width == 0U) crop_width = 1U;
        crop_x = (source->width - crop_width) / 2U;
    } else if (source_ratio < target_ratio) {
        crop_height = (uint32_t)(((uint64_t)source->width * output_height) / output_width);
        if (crop_height == 0U) crop_height = 1U;
        crop_y = (source->height - crop_height) / 2U;
    }

    FILE *file = fopen(path, "wb");
    if (!file) {
        fprintf(stderr, "wallpaper: cannot create %s: %s\n", path, strerror(errno));
        return -1;
    }

    uint8_t header[WALLPAPER_HEADER_SIZE] = {0};
    store_le32(header + 0U, WALLPAPER_MAGIC);
    store_le16(header + 4U, WALLPAPER_VERSION);
    store_le16(header + 6U, WALLPAPER_RGB565);
    store_le32(header + 8U, output_width);
    store_le32(header + 12U, output_height);
    store_le32(header + 16U, output_width);
    store_le32(header + 20U, (uint32_t)payload_size);
    if (write_all(file, header, sizeof(header)) != 0) {
        fprintf(stderr, "wallpaper: failed to write header\n");
        fclose(file);
        remove(path);
        return -1;
    }

    uint8_t *row = malloc((size_t)output_width * 2U);
    if (!row) {
        fprintf(stderr, "wallpaper: out of memory while scaling image\n");
        fclose(file);
        remove(path);
        return -1;
    }

    for (uint32_t y = 0U; y < output_height; y++) {
        uint64_t y_fp = scale_coordinate(crop_y, crop_height, y, output_height);

        for (uint32_t x = 0U; x < output_width; x++) {
            uint64_t x_fp = scale_coordinate(crop_x, crop_width, x, output_width);

            uint8_t rgb[3];
            sample_bilinear(source, x_fp, y_fp, rgb);
            uint16_t pixel = pack_rgb565(rgb);
            row[(size_t)x * 2U] = (uint8_t)pixel;
            row[(size_t)x * 2U + 1U] = (uint8_t)(pixel >> 8U);
        }
        if (write_all(file, row, (size_t)output_width * 2U) != 0) {
            fprintf(stderr, "wallpaper: failed to write pixel data\n");
            free(row);
            fclose(file);
            remove(path);
            return -1;
        }
    }

    free(row);
    if (fclose(file) != 0) {
        fprintf(stderr, "wallpaper: failed to close %s\n", path);
        remove(path);
        return -1;
    }
    return 0;
}

static int parse_dimension(const char *text, uint32_t *value_out) {
    char *end = NULL;
    errno = 0;
    unsigned long value = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value == 0UL || value > UINT32_MAX)
        return -1;
    *value_out = (uint32_t)value;
    return 0;
}

static int self_test_png(void) {
    static const uint8_t source_pixels[12] = {
        255U, 0U, 0U, 0U, 255U, 0U,
        0U, 0U, 255U, 255U, 255U, 255U
    };
    png_image writer;
    memset(&writer, 0, sizeof(writer));
    writer.version = PNG_IMAGE_VERSION;
    writer.width = 2U;
    writer.height = 2U;
    writer.format = PNG_FORMAT_RGB;

    png_alloc_size_t encoded_size = 0U;
    if (!png_image_write_to_memory(&writer, NULL, &encoded_size, 0, source_pixels, 0, NULL))
        return -1;
    void *encoded = malloc((size_t)encoded_size);
    if (!encoded) return -1;
    if (!png_image_write_to_memory(&writer, encoded, &encoded_size, 0, source_pixels, 0, NULL)) {
        free(encoded);
        return -1;
    }

    png_image reader;
    memset(&reader, 0, sizeof(reader));
    reader.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_memory(&reader, encoded, (size_t)encoded_size)) {
        free(encoded);
        return -1;
    }
    reader.format = PNG_FORMAT_RGB;
    uint8_t decoded[12] = {0};
    int ok = png_image_finish_read(&reader, NULL, decoded, 0, NULL) &&
             reader.width == 2U && reader.height == 2U &&
             memcmp(source_pixels, decoded, sizeof(decoded)) == 0;
    png_image_free(&reader);
    free(encoded);
    return ok ? 0 : -1;
}

static int self_test_jpeg(void) {
    static const uint8_t source_pixels[48] = {
        20U, 40U, 60U, 30U, 50U, 70U, 40U, 60U, 80U, 50U, 70U, 90U,
        60U, 80U, 100U, 70U, 90U, 110U, 80U, 100U, 120U, 90U, 110U, 130U,
        100U, 120U, 140U, 110U, 130U, 150U, 120U, 140U, 160U, 130U, 150U, 170U,
        140U, 160U, 180U, 150U, 170U, 190U, 160U, 180U, 200U, 170U, 190U, 210U
    };

    struct jpeg_compress_struct encoder;
    struct jpeg_error_context encoder_error;
    unsigned char *encoded = NULL;
    unsigned long encoded_size = 0UL;
    memset(&encoder, 0, sizeof(encoder));
    memset(&encoder_error, 0, sizeof(encoder_error));
    encoder.err = jpeg_std_error(&encoder_error.base);
    encoder_error.base.error_exit = jpeg_error_exit;
    if (setjmp(encoder_error.jump)) {
        jpeg_destroy_compress(&encoder);
        free(encoded);
        return -1;
    }

    jpeg_create_compress(&encoder);
    jpeg_mem_dest(&encoder, &encoded, &encoded_size);
    encoder.image_width = 4U;
    encoder.image_height = 4U;
    encoder.input_components = 3;
    encoder.in_color_space = JCS_RGB;
    jpeg_set_defaults(&encoder);
    jpeg_set_quality(&encoder, 95, TRUE);
    jpeg_start_compress(&encoder, TRUE);
    while (encoder.next_scanline < encoder.image_height) {
        JSAMPROW row = (JSAMPROW)(source_pixels + (size_t)encoder.next_scanline * 12U);
        if (jpeg_write_scanlines(&encoder, &row, 1U) != 1U) {
            jpeg_destroy_compress(&encoder);
            free(encoded);
            return -1;
        }
    }
    jpeg_finish_compress(&encoder);
    jpeg_destroy_compress(&encoder);

    struct jpeg_decompress_struct decoder;
    struct jpeg_error_context decoder_error;
    struct image_rgb decoded = {0};
    memset(&decoder, 0, sizeof(decoder));
    memset(&decoder_error, 0, sizeof(decoder_error));
    decoder.err = jpeg_std_error(&decoder_error.base);
    decoder_error.base.error_exit = jpeg_error_exit;
    if (setjmp(decoder_error.jump)) {
        jpeg_destroy_decompress(&decoder);
        image_release(&decoded);
        free(encoded);
        return -1;
    }
    jpeg_create_decompress(&decoder);
    jpeg_mem_src(&decoder, encoded, encoded_size);
    int result = decode_jpeg_stream(&decoder, &decoded);
    jpeg_destroy_decompress(&decoder);
    free(encoded);
    if (result != 0 || decoded.width != 4U || decoded.height != 4U) {
        image_release(&decoded);
        return -1;
    }
    image_release(&decoded);
    return 0;
}

static int run_self_test(void) {
    if (self_test_png() != 0) {
        fprintf(stderr, "wallpaper: libpng self-test failed\n");
        return 1;
    }
    if (self_test_jpeg() != 0) {
        fprintf(stderr, "wallpaper: libjpeg-turbo self-test failed\n");
        return 1;
    }
    printf("wallpaper: codec self-test passed (libpng %s, libjpeg-turbo %s, zlib %s)\n",
           PNG_LIBPNG_VER_STRING, TUNIX_LIBJPEG_TURBO_VERSION, TUNIX_ZLIB_VERSION);
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "--version") == 0) {
        printf("tunix-wallpaper 1.0 (libpng %s, libjpeg-turbo %s, zlib %s)\n",
               PNG_LIBPNG_VER_STRING, TUNIX_LIBJPEG_TURBO_VERSION, TUNIX_ZLIB_VERSION);
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "--self-test") == 0) return run_self_test();
    if (argc < 3) {
        usage(argv[0]);
        return 2;
    }

    uint32_t width = 960U;
    uint32_t height = 540U;
    for (int index = 3; index < argc; index++) {
        if (strcmp(argv[index], "--width") == 0 && index + 1 < argc) {
            if (parse_dimension(argv[++index], &width) != 0) {
                fprintf(stderr, "wallpaper: invalid width\n");
                return 2;
            }
        } else if (strcmp(argv[index], "--height") == 0 && index + 1 < argc) {
            if (parse_dimension(argv[++index], &height) != 0) {
                fprintf(stderr, "wallpaper: invalid height\n");
                return 2;
            }
        } else {
            usage(argv[0]);
            return 2;
        }
    }

    struct image_rgb source = {0};
    const char *codec = NULL;
    if (decode_image_file(argv[1], &source, &codec) != 0) return 1;
    if (write_wallpaper(argv[2], &source, width, height) != 0) {
        image_release(&source);
        return 1;
    }
    printf("wallpaper: %s decoded by %s, wrote %s (%ux%u RGB565)\n",
           argv[1], codec, argv[2], width, height);
    image_release(&source);
    return 0;
}
