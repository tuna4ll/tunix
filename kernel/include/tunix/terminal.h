#ifndef TUNIX_TERMINAL_H
#define TUNIX_TERMINAL_H

#include <stdint.h>

struct terminal_screen;

int terminal_init(void);
int terminal_ready(void);

struct terminal_screen *terminal_screen_create(void);
void terminal_screen_destroy(struct terminal_screen *screen);
void terminal_screen_activate(struct terminal_screen *screen);
struct terminal_screen *terminal_screen_active(void);
void terminal_redraw(void);
void terminal_get_dimensions(uint16_t *rows, uint16_t *cols);

void terminal_print(const char *text);

void terminal_paint_begin(void);
void terminal_paint_end(void);
void terminal_paint_lock_reset(void);

void terminal_clear(struct terminal_screen *screen);
void terminal_put_char(struct terminal_screen *screen, char c);
void terminal_put_codepoint(struct terminal_screen *screen, uint32_t codepoint);
void terminal_set_sgr(struct terminal_screen *screen, unsigned code);
void terminal_set_sgr_sequence(struct terminal_screen *screen,
                               const unsigned *codes, unsigned count);
void terminal_cursor_move(struct terminal_screen *screen, int row_delta, int col_delta);
void terminal_cursor_set(struct terminal_screen *screen, int row, int col);
void terminal_cursor_get(struct terminal_screen *screen, int *row, int *col);
void terminal_set_cursor_visible(struct terminal_screen *screen, int visible);
void terminal_set_alternate_screen(struct terminal_screen *screen, int enabled);
void terminal_erase_display(struct terminal_screen *screen, unsigned mode);
void terminal_erase_line(struct terminal_screen *screen, unsigned mode);
void terminal_set_scroll_region(struct terminal_screen *screen, int top, int bottom);
void terminal_insert_lines(struct terminal_screen *screen, unsigned count);
void terminal_delete_lines(struct terminal_screen *screen, unsigned count);
void terminal_insert_chars(struct terminal_screen *screen, unsigned count);
void terminal_delete_chars(struct terminal_screen *screen, unsigned count);
void terminal_erase_chars(struct terminal_screen *screen, unsigned count);
void terminal_scroll_up(struct terminal_screen *screen, unsigned count);
void terminal_scroll_down(struct terminal_screen *screen, unsigned count);

#endif
