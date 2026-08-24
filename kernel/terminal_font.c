#include <stddef.h>
#include <stdint.h>
#include "include/terminal_font.h"
#include "terminal_font_data.inc"

static size_t find_glyph(uint32_t codepoint) {
    size_t first = 0;
    size_t last = TUNIX_TERMINAL_FONT_GLYPH_COUNT;
    while (first < last) {
        size_t middle = first + (last - first) / 2U;
        uint32_t candidate = tunix_terminal_font_codepoints[middle];
        if (candidate < codepoint) first = middle + 1U;
        else last = middle;
    }
    if (first < TUNIX_TERMINAL_FONT_GLYPH_COUNT &&
        tunix_terminal_font_codepoints[first] == codepoint)
        return first;
    return TUNIX_TERMINAL_FONT_GLYPH_COUNT;
}

const uint8_t *tunix_terminal_font_glyph(uint32_t codepoint) {
    size_t index = find_glyph(codepoint);
    if (index == TUNIX_TERMINAL_FONT_GLYPH_COUNT) index = find_glyph(UINT32_C(0xFFFD));
    if (index == TUNIX_TERMINAL_FONT_GLYPH_COUNT) index = find_glyph((uint32_t)'?');
    return tunix_terminal_font_alpha + index * TUNIX_TERMINAL_FONT_PIXELS_PER_GLYPH;
}
