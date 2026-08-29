#ifndef TUNIX_TERMINAL_H
#define TUNIX_TERMINAL_H

#include <stdint.h>

/*
 * A screen is one virtual terminal's picture: the grid of cells, the cursor and
 * the attributes that go with it. Every operation names the screen it acts on,
 * and only the active one reaches the display -- a background terminal keeps
 * scrolling and drawing into its cells, and shows the result the moment it is
 * switched to. That is the whole of what makes a background VT work.
 */
struct terminal_screen;

/* Measures the display and paints it. Called once, before any screen exists. */
int terminal_init(void);
int terminal_ready(void);

struct terminal_screen *terminal_screen_create(void);
void terminal_screen_destroy(struct terminal_screen *screen);
/* Put this screen on the display and paint it. NULL blanks the console, which
   is what happens while a graphics client owns the framebuffer. */
void terminal_screen_activate(struct terminal_screen *screen);
struct terminal_screen *terminal_screen_active(void);
/* Repaint whatever is active, after somebody else has drawn over it. */
void terminal_redraw(void);
/* The same for every screen, since one display means one size. */
void terminal_get_dimensions(uint16_t *rows, uint16_t *cols);

/* Writes to the active screen; the panic path, which has no terminal to hand. */
void terminal_print(const char *text);

/* Hold the screen for a run of characters. Anything writing more than one at a
   time takes these rather than paying for the lock per glyph; nesting on one
   processor is allowed. See terminal.c. */
void terminal_paint_begin(void);
void terminal_paint_end(void);
/* Drop the painting lock. Only panic() has any business calling this: see
   terminal.c. */
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
