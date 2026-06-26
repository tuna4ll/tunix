#ifndef TUNIX_TERMINAL_FONT_H
#define TUNIX_TERMINAL_FONT_H

#include <stdint.h>

#define TUNIX_TERMINAL_FONT_WIDTH 8U
#define TUNIX_TERMINAL_FONT_HEIGHT 13U
#define TUNIX_TERMINAL_FONT_GLYPHS 256U
#define TUNIX_TERMINAL_FONT_BYTES_PER_GLYPH 13U

extern const uint8_t tunix_terminal_font_bitmap[];

#endif
