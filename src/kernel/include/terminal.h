#ifndef TUNIX_TERMINAL_H
#define TUNIX_TERMINAL_H

#include <stdint.h>

int terminal_init(const char *wallpaper_path);
void terminal_clear(void);
void terminal_print(const char *text);
void terminal_put_char(char c);
void terminal_set_sgr(unsigned code);
void terminal_set_sgr_sequence(const unsigned *codes, unsigned count);
void terminal_cursor_move(int row_delta, int col_delta);
void terminal_cursor_set(int row, int col);
void terminal_cursor_get(int *row, int *col);
void terminal_get_dimensions(uint16_t *rows, uint16_t *cols);
void terminal_set_cursor_visible(int visible);
void terminal_set_alternate_screen(int enabled);
void terminal_erase_display(unsigned mode);
void terminal_erase_line(unsigned mode);
void terminal_set_scroll_region(int top, int bottom);
void terminal_insert_lines(unsigned count);
void terminal_delete_lines(unsigned count);
void terminal_insert_chars(unsigned count);
void terminal_delete_chars(unsigned count);
void terminal_erase_chars(unsigned count);
void terminal_scroll_up(unsigned count);
void terminal_scroll_down(unsigned count);
void terminal_print_hex(uint32_t value);

#endif
