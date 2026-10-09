#ifndef TUNIX_TERMINAL_FONT_H
#define TUNIX_TERMINAL_FONT_H

#include <stdint.h>

#define TUNIX_TERMINAL_FONT_WIDTH  8U
#define TUNIX_TERMINAL_FONT_HEIGHT 16U

const uint8_t *tunix_terminal_font_glyph(uint32_t codepoint);

#endif
