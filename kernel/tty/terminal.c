#include <stddef.h>
#include <stdint.h>
#include "../include/framebuffer.h"
#include "../include/smp.h"
#include "../include/heap.h"
#include "../include/kstring.h"
#include "../include/vfs.h"
#include "../include/terminal.h"
#include "../include/terminal_font.h"

#define MAX_CONSOLE_COLS 224
#define MAX_CONSOLE_ROWS 80
#define CELL_BG_EXPLICIT 0x01U
/* The console has no wallpaper: every pixel a cell does not cover is this.
   Keeping it a constant rather than a screen-sized array is what lets each
   virtual terminal cost only its cells. */
#define CONSOLE_BACKGROUND 0x000000U

struct console_cell {
    uint32_t codepoint;
    uint8_t flags;
    uint8_t reserved[3];
    uint32_t foreground;
    uint32_t background;
};

struct console_layout {
    uint32_t screen_width;
    uint32_t screen_height;
    uint32_t content_x;
    uint32_t content_y;
    uint32_t cell_width;
    uint32_t cell_height;
    uint16_t columns;
    uint16_t rows;
};

/*
 * One terminal's picture. `cells` is what is on it; `primary_cells` is the
 * ordinary screen parked while an application is using the alternate one, and
 * is only allocated if something ever asks for that.
 */
struct terminal_screen {
    struct console_cell *cells;
    struct console_cell *primary_cells;
    int col;
    int row;
    uint32_t foreground;
    uint32_t background;
    uint8_t background_explicit;
    uint8_t bold;
    uint8_t reverse;
    int cursor_visible;
    int alternate_active;
    int primary_col;
    int primary_row;
    int scroll_top;
    int scroll_bottom;
    int primary_scroll_top;
    int primary_scroll_bottom;
};

static struct console_layout layout;
static int terminal_is_ready;
static struct terminal_screen *active_screen;

static const uint32_t ansi_palette[16] = {
    0x15161EU, 0xF7768EU, 0x9ECE6AU, 0xE0AF68U,
    0x7AA2F7U, 0xBB9AF7U, 0x7DCFFFU, 0xC0CAF5U,
    0x414868U, 0xFF899DU, 0xB9F27CU, 0xF4C97AU,
    0x8DB0FFU, 0xC7A9FFU, 0x9BE8FFU, 0xF4F7FFU
};

static uint32_t blend_rgb(uint32_t base, uint32_t overlay, uint8_t alpha) {
    uint32_t inverse = 255U - alpha;
    uint32_t red = (((base >> 16) & 0xFFU) * inverse +
                    ((overlay >> 16) & 0xFFU) * alpha) / 255U;
    uint32_t green = (((base >> 8) & 0xFFU) * inverse +
                      ((overlay >> 8) & 0xFFU) * alpha) / 255U;
    uint32_t blue = ((base & 0xFFU) * inverse +
                     (overlay & 0xFFU) * alpha) / 255U;
    return (red << 16) | (green << 8) | blue;
}

static uint32_t shade_rgb(uint32_t color, uint32_t numerator, uint32_t denominator) {
    uint32_t red = (((color >> 16) & 0xFFU) * numerator) / denominator;
    uint32_t green = (((color >> 8) & 0xFFU) * numerator) / denominator;
    uint32_t blue = ((color & 0xFFU) * numerator) / denominator;
    if (red > 255U) red = 255U;
    if (green > 255U) green = 255U;
    if (blue > 255U) blue = 255U;
    return (red << 16) | (green << 8) | blue;
}

static void fill_background_rect(uint32_t x, uint32_t y, uint32_t width, uint32_t height) {
    uint32_t end_x = x + width;
    uint32_t end_y = y + height;
    if (end_x > layout.screen_width) end_x = layout.screen_width;
    if (end_y > layout.screen_height) end_y = layout.screen_height;
    for (uint32_t py = y; py < end_y; py++) {
        for (uint32_t px = x; px < end_x; px++)
            framebuffer_put_rgb(px, py, CONSOLE_BACKGROUND);
    }
}

static void calculate_layout(void) {
    /* Plain full-screen text console (VGA-text style): no floating window, no
       margins beyond a small edge gap, text fills the whole framebuffer. */
    layout.screen_width = framebuffer_width();
    layout.screen_height = framebuffer_height();
    uint32_t margin = 8U;
    layout.content_x = margin;
    layout.content_y = margin;
    layout.cell_width = TUNIX_TERMINAL_FONT_WIDTH;
    layout.cell_height = TUNIX_TERMINAL_FONT_HEIGHT;
    uint32_t columns = (layout.screen_width - margin * 2U) / layout.cell_width;
    uint32_t rows = (layout.screen_height - margin * 2U) / layout.cell_height;
    if (columns > MAX_CONSOLE_COLS) columns = MAX_CONSOLE_COLS;
    if (rows > MAX_CONSOLE_ROWS) rows = MAX_CONSOLE_ROWS;
    if (columns < 40U) columns = 40U;
    if (rows < 16U) rows = 16U;
    layout.columns = (uint16_t)columns;
    layout.rows = (uint16_t)rows;
}

static size_t cell_count(void) {
    return (size_t)layout.columns * layout.rows;
}

static struct console_cell *cell_at(struct terminal_screen *screen, int row, int col) {
    return &screen->cells[(size_t)row * layout.columns + (size_t)col];
}

/* Is this screen the one the display is showing? A background terminal answers
   no, and so does every terminal while a graphics client owns the scanout. */
static int visible(const struct terminal_screen *screen) {
    return terminal_is_ready && screen == active_screen && framebuffer_console_active();
}

static void reset_attributes(struct terminal_screen *screen) {
    screen->foreground = 0xD8DEE9U;
    screen->background = 0x000000U;
    screen->background_explicit = 0;
    screen->bold = 0;
    screen->reverse = 0;
}

static void clear_cell_model(struct terminal_screen *screen) {
    memset(screen->cells, 0, cell_count() * sizeof(screen->cells[0]));
}

static void clear_row_range(struct terminal_screen *screen, int row,
                            int first_col, int last_col) {
    if (row < 0 || row >= layout.rows) return;
    if (first_col < 0) first_col = 0;
    if (last_col >= layout.columns) last_col = layout.columns - 1;
    if (first_col > last_col) return;
    memset(cell_at(screen, row, first_col), 0,
           (size_t)(last_col - first_col + 1) * sizeof(screen->cells[0]));
}

static unsigned scroll_region_up(struct terminal_screen *screen, int top,
                                 int bottom, unsigned count) {
    if (top < 0) top = 0;
    if (bottom >= layout.rows) bottom = layout.rows - 1;
    if (top > bottom) return 0;
    unsigned height = (unsigned)(bottom - top + 1);
    if (!count) count = 1;
    if (count > height) count = height;
    size_t row_bytes = (size_t)layout.columns * sizeof(screen->cells[0]);
    unsigned remaining = height - count;
    if (remaining) {
        memmove(cell_at(screen, top, 0), cell_at(screen, top + (int)count, 0),
                (size_t)remaining * row_bytes);
    }
    memset(cell_at(screen, bottom - (int)count + 1, 0), 0, (size_t)count * row_bytes);
    return count;
}

static unsigned scroll_region_down(struct terminal_screen *screen, int top,
                                   int bottom, unsigned count) {
    if (top < 0) top = 0;
    if (bottom >= layout.rows) bottom = layout.rows - 1;
    if (top > bottom) return 0;
    unsigned height = (unsigned)(bottom - top + 1);
    if (!count) count = 1;
    if (count > height) count = height;
    size_t row_bytes = (size_t)layout.columns * sizeof(screen->cells[0]);
    unsigned remaining = height - count;
    if (remaining) {
        memmove(cell_at(screen, top + (int)count, 0), cell_at(screen, top, 0),
                (size_t)remaining * row_bytes);
    }
    memset(cell_at(screen, top, 0), 0, (size_t)count * row_bytes);
    return count;
}

static void clear_cell_background(int row, int col) {
    uint32_t x = layout.content_x + (uint32_t)col * layout.cell_width;
    uint32_t y = layout.content_y + (uint32_t)row * layout.cell_height;
    fill_background_rect(x, y, layout.cell_width, layout.cell_height);
}

static void draw_glyph_to_framebuffer(uint32_t x, uint32_t y, uint32_t codepoint,
                                      uint32_t color, uint32_t background) {
    const uint8_t *glyph = tunix_terminal_font_glyph(codepoint);
    for (uint32_t row = 0; row < TUNIX_TERMINAL_FONT_HEIGHT; row++) {
        for (uint32_t column = 0; column < TUNIX_TERMINAL_FONT_WIDTH; column++) {
            uint8_t alpha = glyph[row * TUNIX_TERMINAL_FONT_WIDTH + column];
            if (!alpha) continue;
            framebuffer_put_rgb(x + column, y + row,
                                blend_rgb(background, color, alpha));
        }
    }
}

static void draw_cell_overlay(struct terminal_screen *screen, int row, int col,
                              int cursor) {
    struct console_cell *cell = cell_at(screen, row, col);
    uint32_t x = layout.content_x + (uint32_t)col * layout.cell_width;
    uint32_t y = layout.content_y + (uint32_t)row * layout.cell_height;
    if (cell->flags & CELL_BG_EXPLICIT) {
        for (uint32_t py = 0; py < layout.cell_height; py++) {
            for (uint32_t px = 0; px < layout.cell_width; px++)
                framebuffer_put_rgb(x + px, y + py, cell->background);
        }
    }
    if (cell->codepoint >= 32U && cell->codepoint != 127U)
        draw_glyph_to_framebuffer(x, y, cell->codepoint, cell->foreground,
                                  (cell->flags & CELL_BG_EXPLICIT)
                                      ? cell->background : CONSOLE_BACKGROUND);
    if (cursor) {
        uint32_t cursor_color = 0x7DCFFFU;
        for (uint32_t py = layout.cell_height - 2U; py < layout.cell_height; py++) {
            for (uint32_t px = 0; px < layout.cell_width; px++)
                framebuffer_put_rgb(x + px, y + py, cursor_color);
        }
    }
}

static void render_cell(struct terminal_screen *screen, int row, int col, int cursor) {
    if (!visible(screen)) return;
    if (row < 0 || col < 0 || row >= layout.rows || col >= layout.columns) return;
    clear_cell_background(row, col);
    draw_cell_overlay(screen, row, col, cursor);
}

static void render_console(struct terminal_screen *screen) {
    if (!visible(screen)) return;
    uint32_t width = (uint32_t)layout.columns * layout.cell_width;
    uint32_t height = (uint32_t)layout.rows * layout.cell_height;
    fill_background_rect(layout.content_x, layout.content_y, width, height);
    for (int row = 0; row < layout.rows; row++) {
        for (int col = 0; col < layout.columns; col++) {
            struct console_cell *cell = cell_at(screen, row, col);
            if (cell->codepoint || (cell->flags & CELL_BG_EXPLICIT))
                draw_cell_overlay(screen, row, col, 0);
        }
    }
    if (screen->cursor_visible) draw_cell_overlay(screen, screen->row, screen->col, 1);
}

static void render_region_scroll_up(struct terminal_screen *screen, int top,
                                    int bottom, unsigned count) {
    if (!visible(screen)) return;
    if (!count || top < 0 || bottom >= layout.rows || top > bottom) return;
    uint32_t x = layout.content_x;
    uint32_t y = layout.content_y + (uint32_t)top * layout.cell_height;
    uint32_t width = (uint32_t)layout.columns * layout.cell_width;
    uint32_t height = (uint32_t)(bottom - top + 1) * layout.cell_height;
    uint32_t shift = count * layout.cell_height;
    if (shift < height) {
        framebuffer_copy_rect(x, y, x, y + shift, width, height - shift);
    }
    fill_background_rect(x, y + height - shift, width, shift);
}

static void render_region_scroll_down(struct terminal_screen *screen, int top,
                                      int bottom, unsigned count) {
    if (!visible(screen)) return;
    if (!count || top < 0 || bottom >= layout.rows || top > bottom) return;
    uint32_t x = layout.content_x;
    uint32_t y = layout.content_y + (uint32_t)top * layout.cell_height;
    uint32_t width = (uint32_t)layout.columns * layout.cell_width;
    uint32_t height = (uint32_t)(bottom - top + 1) * layout.cell_height;
    uint32_t shift = count * layout.cell_height;
    if (shift < height) {
        framebuffer_copy_rect(x, y + shift, x, y, width, height - shift);
    }
    fill_background_rect(x, y, width, shift);
}

static void erase_visible_cursor(struct terminal_screen *screen) {
    if (screen->cursor_visible) render_cell(screen, screen->row, screen->col, 0);
}

static void redraw_visible_cursor(struct terminal_screen *screen) {
    if (screen->cursor_visible) render_cell(screen, screen->row, screen->col, 1);
}

static void clamp_terminal_cursor(struct terminal_screen *screen) {
    if (screen->row < 0) screen->row = 0;
    if (screen->col < 0) screen->col = 0;
    if (screen->row >= layout.rows) screen->row = layout.rows - 1;
    if (screen->col >= layout.columns) screen->col = layout.columns - 1;
}

static void terminal_scroll_if_needed(struct terminal_screen *screen) {
    if (screen->row == screen->scroll_bottom + 1) {
        unsigned count = scroll_region_up(screen, screen->scroll_top,
                                          screen->scroll_bottom, 1);
        screen->row = screen->scroll_bottom;
        render_region_scroll_up(screen, screen->scroll_top, screen->scroll_bottom, count);
    } else if (screen->row >= layout.rows) {
        screen->row = layout.rows - 1;
    }
}

static uint32_t xterm_256_color(unsigned index) {
    if (index < 16U) return ansi_palette[index];
    if (index < 232U) {
        unsigned value = index - 16U;
        unsigned red = value / 36U;
        unsigned green = (value / 6U) % 6U;
        unsigned blue = value % 6U;
        static const uint8_t levels[6] = {0, 95, 135, 175, 215, 255};
        return ((uint32_t)levels[red] << 16) |
               ((uint32_t)levels[green] << 8) | levels[blue];
    }
    unsigned gray = 8U + (index - 232U) * 10U;
    return (gray << 16) | (gray << 8) | gray;
}

int terminal_init(void) {
    if (!framebuffer_available()) return -1;
    calculate_layout();
    fill_background_rect(0, 0, layout.screen_width, layout.screen_height);
    active_screen = NULL;
    terminal_is_ready = 1;
    return 0;
}

int terminal_ready(void) { return terminal_is_ready; }

struct terminal_screen *terminal_screen_create(void) {
    if (!terminal_is_ready) return NULL;
    struct terminal_screen *screen = kmalloc(sizeof(*screen));
    if (!screen) return NULL;
    memset(screen, 0, sizeof(*screen));
    screen->cells = kmalloc(cell_count() * sizeof(struct console_cell));
    if (!screen->cells) {
        kfree(screen);
        return NULL;
    }
    clear_cell_model(screen);
    reset_attributes(screen);
    screen->cursor_visible = 1;
    screen->scroll_top = 0;
    screen->scroll_bottom = layout.rows - 1;
    return screen;
}

void terminal_screen_destroy(struct terminal_screen *screen) {
    if (!screen) return;
    if (screen == active_screen) active_screen = NULL;
    kfree(screen->cells);
    kfree(screen->primary_cells);
    kfree(screen);
}

void terminal_screen_activate(struct terminal_screen *screen) {
    if (!terminal_is_ready) return;
    active_screen = screen;
    if (!framebuffer_console_active()) return;
    /* The whole display, not just the text area: what was there belonged to
       another terminal, or to a graphics client that has just stood down. */
    fill_background_rect(0, 0, layout.screen_width, layout.screen_height);
    if (screen) render_console(screen);
}

struct terminal_screen *terminal_screen_active(void) { return active_screen; }

void terminal_redraw(void) {
    if (!terminal_is_ready || !active_screen) return;
    render_console(active_screen);
}

void terminal_get_dimensions(uint16_t *rows, uint16_t *cols) {
    if (rows) *rows = terminal_is_ready ? layout.rows : 0;
    if (cols) *cols = terminal_is_ready ? layout.columns : 0;
}

void terminal_clear(struct terminal_screen *screen) {
    if (!terminal_is_ready || !screen) return;
    clear_cell_model(screen);
    screen->col = 0;
    screen->row = 0;
    render_console(screen);
}

void terminal_put_codepoint(struct terminal_screen *screen, uint32_t codepoint) {
    if (!terminal_is_ready || !screen) return;
    if (codepoint > UINT32_C(0x10FFFF) ||
        (codepoint >= UINT32_C(0xD800) && codepoint <= UINT32_C(0xDFFF)))
        codepoint = UINT32_C(0xFFFD);

    render_cell(screen, screen->row, screen->col, 0);
    if (codepoint == '\n') {
        screen->col = 0;
        screen->row++;
    } else if (codepoint == '\r') {
        screen->col = 0;
    } else if (codepoint == '\b' || codepoint == 127U) {
        if (screen->col > 0) screen->col--;
        else if (screen->row > 0) {
            screen->row--;
            screen->col = layout.columns - 1;
        }
        render_cell(screen, screen->row, screen->col, 0);
    } else if (codepoint == '\t') {
        int spaces = 8 - (screen->col & 7);
        while (spaces-- > 0) terminal_put_codepoint(screen, ' ');
        return;
    } else if (codepoint >= 32U) {
        struct console_cell *cell = cell_at(screen, screen->row, screen->col);
        uint32_t foreground = screen->foreground;
        uint32_t background = screen->background;
        uint8_t explicit_background = screen->background_explicit;
        if (screen->reverse) {
            uint32_t temporary = foreground;
            foreground = explicit_background ? background : 0x000000U;
            background = temporary;
            explicit_background = 1;
        }
        cell->codepoint = codepoint;
        cell->foreground = screen->bold ? shade_rgb(foreground, 116U, 100U) : foreground;
        cell->background = background;
        cell->flags = explicit_background ? CELL_BG_EXPLICIT : 0U;
        render_cell(screen, screen->row, screen->col, 0);
        screen->col++;
        if (screen->col >= layout.columns) {
            screen->col = 0;
            screen->row++;
        }
    }
    terminal_scroll_if_needed(screen);
    if (screen->cursor_visible) render_cell(screen, screen->row, screen->col, 1);
}

/*
 * One processor paints at a time.
 *
 * Two write into the same screen from two directions: a terminal a program is
 * writing to, under the kernel lock, and the kernel log, which is not. Neither
 * cell model nor cursor survives that being interleaved -- what it looks like
 * from the front is two messages spliced into each other a character at a time
 * and coloured rubbish where a scroll got half done.
 *
 * Interrupts go off with it because kprintf() is reachable from an interrupt
 * handler, and a processor that took one while holding this would wait for
 * itself.
 */
static volatile int paint_lock;

static uint64_t paint_acquire(void) {
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(flags) :: "memory");
    /* Servicing the flush inside the spin is not an optimisation. A processor
       waiting here has interrupts off, so the one asking it to drop cached
       translations cannot reach it as an interrupt -- and that processor is
       very likely holding the kernel lock while it waits, which is a machine
       that stops with one processor holding everything. */
    while (__atomic_test_and_set(&paint_lock, __ATOMIC_ACQUIRE)) {
        smp_service_flush();
        __asm__ volatile("pause");
    }
    return flags;
}

static void paint_release(uint64_t flags) {
    __atomic_clear(&paint_lock, __ATOMIC_RELEASE);
    if (flags & 0x200ULL) __asm__ volatile("sti");
}

/*
 * Held across a run of characters, not around each one.
 *
 * Taking it per character was correct and far too expensive: interrupts off,
 * a contended cache line and a released lock for every glyph, which on the
 * boot messages alone is tens of thousands of times. What it produced was a
 * processor holding the kernel lock for seconds at a stretch while it painted
 * -- the lock watchdog saw it before anything else did.
 */
static uint64_t paint_flags;
static unsigned paint_depth;

void terminal_paint_begin(void) {
    uint64_t flags = paint_acquire();
    /* Only the outermost caller owns the saved flags: the inner ones took
       nothing, because the lock is already this processor's. */
    if (paint_depth++ == 0) paint_flags = flags;
}

void terminal_paint_end(void) {
    if (paint_depth == 0) return;
    if (--paint_depth == 0) paint_release(paint_flags);
}

void terminal_put_char(struct terminal_screen *screen, char c) {
    terminal_put_codepoint(screen, (uint8_t)c);
}

void terminal_print(const char *text) {
    terminal_paint_begin();
    while (text && *text) terminal_put_codepoint(active_screen, (uint8_t)*text++);
    terminal_paint_end();
}

/* Panic runs after something has already gone wrong, and the processor that
   held this may be the one that went wrong. */
void terminal_paint_lock_reset(void) {
    paint_depth = 0;
    __atomic_clear(&paint_lock, __ATOMIC_RELEASE);
}

void terminal_set_sgr_sequence(struct terminal_screen *screen,
                               const unsigned *codes, unsigned count) {
    if (!terminal_is_ready || !screen) return;
    if (!codes || !count) {
        reset_attributes(screen);
        return;
    }

    for (unsigned i = 0; i < count; i++) {
        unsigned code = codes[i];
        if (code == 0U) reset_attributes(screen);
        else if (code == 1U) screen->bold = 1;
        else if (code == 22U) screen->bold = 0;
        else if (code == 7U) screen->reverse = 1;
        else if (code == 27U) screen->reverse = 0;
        else if (code >= 30U && code <= 37U)
            screen->foreground = ansi_palette[code - 30U];
        else if (code >= 90U && code <= 97U)
            screen->foreground = ansi_palette[8U + code - 90U];
        else if (code == 39U) screen->foreground = 0xD8DEE9U;
        else if (code >= 40U && code <= 47U) {
            screen->background = ansi_palette[code - 40U];
            screen->background_explicit = 1;
        } else if (code >= 100U && code <= 107U) {
            screen->background = ansi_palette[8U + code - 100U];
            screen->background_explicit = 1;
        } else if (code == 49U) {
            screen->background = 0x000000U;
            screen->background_explicit = 0;
        } else if ((code == 38U || code == 48U) && i + 1U < count) {
            uint32_t color = 0;
            int valid = 0;
            if (codes[i + 1U] == 5U && i + 2U < count) {
                color = xterm_256_color(codes[i + 2U] & 0xFFU);
                i += 2U;
                valid = 1;
            } else if (codes[i + 1U] == 2U && i + 4U < count) {
                if (i + 5U < count && codes[i + 2U] == 0U) {
                    color = ((codes[i + 3U] & 0xFFU) << 16) |
                            ((codes[i + 4U] & 0xFFU) << 8) |
                            (codes[i + 5U] & 0xFFU);
                    i += 5U;
                } else {
                    color = ((codes[i + 2U] & 0xFFU) << 16) |
                            ((codes[i + 3U] & 0xFFU) << 8) |
                            (codes[i + 4U] & 0xFFU);
                    i += 4U;
                }
                valid = 1;
            }
            if (valid && code == 38U) screen->foreground = color;
            else if (valid) {
                screen->background = color;
                screen->background_explicit = 1;
            }
        }
    }
}

void terminal_set_sgr(struct terminal_screen *screen, unsigned code) {
    terminal_set_sgr_sequence(screen, &code, 1);
}

void terminal_cursor_move(struct terminal_screen *screen, int row_delta, int col_delta) {
    if (!terminal_is_ready || !screen) return;
    render_cell(screen, screen->row, screen->col, 0);
    screen->row += row_delta;
    screen->col += col_delta;
    clamp_terminal_cursor(screen);
    if (screen->cursor_visible) render_cell(screen, screen->row, screen->col, 1);
}

void terminal_cursor_set(struct terminal_screen *screen, int row, int col) {
    if (!terminal_is_ready || !screen) return;
    render_cell(screen, screen->row, screen->col, 0);
    screen->row = row;
    screen->col = col;
    clamp_terminal_cursor(screen);
    if (screen->cursor_visible) render_cell(screen, screen->row, screen->col, 1);
}

void terminal_cursor_get(struct terminal_screen *screen, int *row, int *col) {
    if (row) *row = screen ? screen->row : 0;
    if (col) *col = screen ? screen->col : 0;
}

void terminal_set_cursor_visible(struct terminal_screen *screen, int visible_now) {
    visible_now = visible_now ? 1 : 0;
    if (!terminal_is_ready || !screen || visible_now == screen->cursor_visible) return;
    render_cell(screen, screen->row, screen->col, 0);
    screen->cursor_visible = visible_now;
    if (screen->cursor_visible) render_cell(screen, screen->row, screen->col, 1);
}

void terminal_set_alternate_screen(struct terminal_screen *screen, int enabled) {
    enabled = enabled ? 1 : 0;
    if (!terminal_is_ready || !screen || enabled == screen->alternate_active) return;
    size_t count = cell_count();
    if (enabled) {
        /* Allocated on the first application that asks for it, which on a
           machine with several terminals is usually none of them. */
        if (!screen->primary_cells) {
            screen->primary_cells = kmalloc(count * sizeof(struct console_cell));
            if (!screen->primary_cells) return;
        }
        memcpy(screen->primary_cells, screen->cells, count * sizeof(struct console_cell));
        screen->primary_col = screen->col;
        screen->primary_row = screen->row;
        screen->primary_scroll_top = screen->scroll_top;
        screen->primary_scroll_bottom = screen->scroll_bottom;
        clear_cell_model(screen);
        screen->col = 0;
        screen->row = 0;
        screen->scroll_top = 0;
        screen->scroll_bottom = layout.rows - 1;
        screen->alternate_active = 1;
    } else {
        if (!screen->primary_cells) return;
        memcpy(screen->cells, screen->primary_cells, count * sizeof(struct console_cell));
        screen->col = screen->primary_col;
        screen->row = screen->primary_row;
        screen->scroll_top = screen->primary_scroll_top;
        screen->scroll_bottom = screen->primary_scroll_bottom;
        clamp_terminal_cursor(screen);
        screen->alternate_active = 0;
    }
    render_console(screen);
}

void terminal_erase_display(struct terminal_screen *screen, unsigned mode) {
    if (!terminal_is_ready || !screen) return;
    if (mode == 2U || mode == 3U) {
        terminal_clear(screen);
        return;
    }
    if (mode == 0U) {
        for (int row = screen->row; row < layout.rows; row++) {
            int start = row == screen->row ? screen->col : 0;
            clear_row_range(screen, row, start, layout.columns - 1);
        }
    } else if (mode == 1U) {
        for (int row = 0; row <= screen->row; row++) {
            int end = row == screen->row ? screen->col : layout.columns - 1;
            clear_row_range(screen, row, 0, end);
        }
    }
    render_console(screen);
}

void terminal_erase_line(struct terminal_screen *screen, unsigned mode) {
    if (!terminal_is_ready || !screen) return;
    int start = 0;
    int end = layout.columns - 1;
    if (mode == 0U) start = screen->col;
    if (mode == 1U) end = screen->col;
    clear_row_range(screen, screen->row, start, end);
    render_console(screen);
}

void terminal_set_scroll_region(struct terminal_screen *screen, int top, int bottom) {
    if (!terminal_is_ready || !screen) return;
    if (top <= 0 && bottom <= 0) {
        screen->scroll_top = 0;
        screen->scroll_bottom = layout.rows - 1;
    } else {
        if (top <= 0) top = 1;
        if (bottom <= 0) bottom = layout.rows;
        top--;
        bottom--;
        if (top < 0 || bottom < 0 || top >= bottom || bottom >= layout.rows) return;
        screen->scroll_top = top;
        screen->scroll_bottom = bottom;
    }
    terminal_cursor_set(screen, 0, 0);
}

void terminal_insert_lines(struct terminal_screen *screen, unsigned count) {
    if (!terminal_is_ready || !screen) return;
    if (screen->row < screen->scroll_top || screen->row > screen->scroll_bottom) return;
    erase_visible_cursor(screen);
    unsigned actual = scroll_region_down(screen, screen->row, screen->scroll_bottom, count);
    render_region_scroll_down(screen, screen->row, screen->scroll_bottom, actual);
    redraw_visible_cursor(screen);
}

void terminal_delete_lines(struct terminal_screen *screen, unsigned count) {
    if (!terminal_is_ready || !screen) return;
    if (screen->row < screen->scroll_top || screen->row > screen->scroll_bottom) return;
    erase_visible_cursor(screen);
    unsigned actual = scroll_region_up(screen, screen->row, screen->scroll_bottom, count);
    render_region_scroll_up(screen, screen->row, screen->scroll_bottom, actual);
    redraw_visible_cursor(screen);
}

void terminal_insert_chars(struct terminal_screen *screen, unsigned count) {
    if (!terminal_is_ready || !screen) return;
    if (!count) count = 1;
    unsigned available = (unsigned)(layout.columns - screen->col);
    if (count > available) count = available;
    unsigned remaining = available - count;
    if (remaining) {
        memmove(cell_at(screen, screen->row, screen->col + (int)count),
                cell_at(screen, screen->row, screen->col),
                (size_t)remaining * sizeof(struct console_cell));
    }
    clear_row_range(screen, screen->row, screen->col, screen->col + (int)count - 1);
    render_console(screen);
}

void terminal_delete_chars(struct terminal_screen *screen, unsigned count) {
    if (!terminal_is_ready || !screen) return;
    if (!count) count = 1;
    unsigned available = (unsigned)(layout.columns - screen->col);
    if (count > available) count = available;
    unsigned remaining = available - count;
    if (remaining) {
        memmove(cell_at(screen, screen->row, screen->col),
                cell_at(screen, screen->row, screen->col + (int)count),
                (size_t)remaining * sizeof(struct console_cell));
    }
    clear_row_range(screen, screen->row, layout.columns - (int)count, layout.columns - 1);
    render_console(screen);
}

void terminal_erase_chars(struct terminal_screen *screen, unsigned count) {
    if (!terminal_is_ready || !screen) return;
    if (!count) count = 1;
    int end = screen->col + (int)count - 1;
    if (end >= layout.columns) end = layout.columns - 1;
    clear_row_range(screen, screen->row, screen->col, end);
    render_console(screen);
}

void terminal_scroll_up(struct terminal_screen *screen, unsigned count) {
    if (!terminal_is_ready || !screen) return;
    erase_visible_cursor(screen);
    unsigned actual = scroll_region_up(screen, screen->scroll_top,
                                       screen->scroll_bottom, count);
    render_region_scroll_up(screen, screen->scroll_top, screen->scroll_bottom, actual);
    redraw_visible_cursor(screen);
}

void terminal_scroll_down(struct terminal_screen *screen, unsigned count) {
    if (!terminal_is_ready || !screen) return;
    erase_visible_cursor(screen);
    unsigned actual = scroll_region_down(screen, screen->scroll_top,
                                         screen->scroll_bottom, count);
    render_region_scroll_down(screen, screen->scroll_top, screen->scroll_bottom, actual);
    redraw_visible_cursor(screen);
}
