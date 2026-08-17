#include <stddef.h>
#include <stdint.h>
#include "include/heap.h"
#include "include/io.h"
#include "include/input.h"
#include "include/kstring.h"
#include "include/process.h"
#include "include/signal.h"
#include "include/tty.h"
#include "include/terminal.h"
#include "include/vt.h"
#include "../include/tunix/input_event.h"

#define EINTR 4
#define EAGAIN 11
#define TTY_INPUT_CAPACITY 1024
#define TTY_CANONICAL_CAPACITY 1024
#define ANSI_PARAM_MAX 16

extern void serial_write_char(char c);

/*
 * One virtual terminal's line discipline and output parser. Everything here is
 * per-terminal: what has been typed at it, what it is echoing, where its cursor
 * is. The keyboard itself is not -- there is one of those, and the state it
 * holds (which modifiers are down) is the file-scope block further down.
 */
struct tty {
    struct terminal_screen *screen;
    struct tunix_termios termios;
    int foreground_pgid;
    /* The session that has claimed this terminal as its controlling terminal,
       or 0 while nobody has. Job control only applies inside that session: a
       process from anywhere else -- login before it has claimed the terminal, a
       service writing to a console -- is simply allowed to read. */
    uint64_t session;

    uint8_t input_buffer[TTY_INPUT_CAPACITY];
    size_t input_head;
    size_t input_tail;
    size_t input_count;
    volatile int input_interrupted;

    char canonical_buffer[TTY_CANONICAL_CAPACITY];
    size_t canonical_length;
    size_t canonical_offset;

    int ansi_state;
    unsigned ansi_params[ANSI_PARAM_MAX];
    unsigned ansi_param_count;
    unsigned ansi_current;
    int ansi_have_current;
    int ansi_private;
    int saved_row;
    int saved_col;
    uint32_t utf8_codepoint;
    uint32_t utf8_minimum;
    unsigned utf8_remaining;
};

static int shift_down;
static int ctrl_down;
static int alt_down;
static int altgr_down;
static int caps_lock;


static const char default_keymap[128] = {
    [0x02]='1',[0x03]='2',[0x04]='3',[0x05]='4',[0x06]='5',[0x07]='6',[0x08]='7',[0x09]='8',[0x0A]='9',[0x0B]='0',
    [0x0C]='-',[0x0D]='=',[0x0E]='\b',[0x0F]='\t',[0x10]='q',[0x11]='w',[0x12]='e',[0x13]='r',[0x14]='t',[0x15]='y',
    [0x16]='u',[0x17]='i',[0x18]='o',[0x19]='p',[0x1A]='[',[0x1B]=']',[0x1C]='\r',[0x1E]='a',[0x1F]='s',[0x20]='d',
    [0x21]='f',[0x22]='g',[0x23]='h',[0x24]='j',[0x25]='k',[0x26]='l',[0x27]=';',[0x28]='\'',[0x29]='`',[0x2B]='\\',
    [0x2C]='z',[0x2D]='x',[0x2E]='c',[0x2F]='v',[0x30]='b',[0x31]='n',[0x32]='m',[0x33]=',',[0x34]='.',[0x35]='/',[0x39]=' '
};

static const char default_shift_keymap[128] = {
    [0x02]='!',[0x03]='@',[0x04]='#',[0x05]='$',[0x06]='%',[0x07]='^',[0x08]='&',[0x09]='*',[0x0A]='(',[0x0B]=')',
    [0x0C]='_',[0x0D]='+',[0x10]='Q',[0x11]='W',[0x12]='E',[0x13]='R',[0x14]='T',[0x15]='Y',[0x16]='U',[0x17]='I',
    [0x18]='O',[0x19]='P',[0x1A]='{',[0x1B]='}',[0x1E]='A',[0x1F]='S',[0x20]='D',[0x21]='F',[0x22]='G',[0x23]='H',
    [0x24]='J',[0x25]='K',[0x26]='L',[0x27]=':',[0x28]='"',[0x29]='~',[0x2B]='|',[0x2C]='Z',[0x2D]='X',[0x2E]='C',
    [0x2F]='V',[0x30]='B',[0x31]='N',[0x32]='M',[0x33]='<',[0x34]='>',[0x35]='?',[0x39]=' '
};

/* One keyboard, one layout: loadkeys(1) changes it for every terminal at once,
   which is what a user of a machine with several of them expects. */
static struct tunix_keymap active_keymap;
static int keymap_loaded;

static int input_push(struct tty *tty, uint8_t value);

static int keymap_is_letter(unsigned keycode) {
    return (active_keymap.letter_bitmap[keycode >> 3] &
            (uint8_t)(1U << (keycode & 7U))) != 0;
}

static void keymap_mark_letter(unsigned keycode) {
    active_keymap.letter_bitmap[keycode >> 3] |=
        (uint8_t)(1U << (keycode & 7U));
}

static void keymap_set_name(const char *name) {
    size_t i = 0;
    while (i + 1U < sizeof(active_keymap.name) && name[i]) {
        active_keymap.name[i] = name[i];
        i++;
    }
    active_keymap.name[i] = 0;
    while (++i < sizeof(active_keymap.name)) active_keymap.name[i] = 0;
}

static void keymap_load_default(void) {
    memset(&active_keymap, 0, sizeof(active_keymap));
    active_keymap.version = TUNIX_KEYMAP_ABI_VERSION;
    keymap_set_name("us");
    for (unsigned level = 0; level < TUNIX_KEYMAP_LEVELS; level++)
        for (unsigned keycode = 0; keycode < TUNIX_KEYMAP_KEYCODES; keycode++)
            active_keymap.symbols[level][keycode] = TUNIX_KEYSYM_NONE;
    for (unsigned keycode = 0; keycode < TUNIX_KEYMAP_KEYCODES; keycode++) {
        if (default_keymap[keycode])
            active_keymap.symbols[0][keycode] = (uint8_t)default_keymap[keycode];
        if (default_shift_keymap[keycode])
            active_keymap.symbols[TUNIX_KEYMAP_LEVEL_SHIFT][keycode] =
                (uint8_t)default_shift_keymap[keycode];
        if (default_keymap[keycode] >= 'a' && default_keymap[keycode] <= 'z')
            keymap_mark_letter(keycode);
    }
    keymap_loaded = 1;
}

static int keymap_valid_codepoint(uint32_t value) {
    if (value == TUNIX_KEYSYM_NONE) return 1;
    if (value > 0x10FFFFU) return 0;
    return value < 0xD800U || value > 0xDFFFU;
}

static int keymap_validate(const struct tunix_keymap *map) {
    if (!map || map->version != TUNIX_KEYMAP_ABI_VERSION) return -1;
    int terminated = 0;
    for (size_t i = 0; i < sizeof(map->name); i++) {
        if (map->name[i] == 0) {
            terminated = 1;
            break;
        }
    }
    if (!terminated) return -1;
    for (unsigned level = 0; level < TUNIX_KEYMAP_LEVELS; level++)
        for (unsigned keycode = 0; keycode < TUNIX_KEYMAP_KEYCODES; keycode++)
            if (!keymap_valid_codepoint(map->symbols[level][keycode])) return -1;
    return 0;
}

static int input_push_codepoint(struct tty *tty, uint32_t value) {
    uint8_t encoded[4];
    unsigned count;
    if (value <= 0x7FU) {
        encoded[0] = (uint8_t)value;
        count = 1;
    } else if (value <= 0x7FFU) {
        encoded[0] = (uint8_t)(0xC0U | (value >> 6));
        encoded[1] = (uint8_t)(0x80U | (value & 0x3FU));
        count = 2;
    } else if (value <= 0xFFFFU) {
        encoded[0] = (uint8_t)(0xE0U | (value >> 12));
        encoded[1] = (uint8_t)(0x80U | ((value >> 6) & 0x3FU));
        encoded[2] = (uint8_t)(0x80U | (value & 0x3FU));
        count = 3;
    } else {
        encoded[0] = (uint8_t)(0xF0U | (value >> 18));
        encoded[1] = (uint8_t)(0x80U | ((value >> 12) & 0x3FU));
        encoded[2] = (uint8_t)(0x80U | ((value >> 6) & 0x3FU));
        encoded[3] = (uint8_t)(0x80U | (value & 0x3FU));
        count = 4;
    }
    if (tty->input_count + count > TTY_INPUT_CAPACITY) return -1;
    for (unsigned i = 0; i < count; i++) (void)input_push(tty, encoded[i]);
    return 0;
}

static uint32_t keymap_lookup(unsigned keycode, unsigned level, int *direct_ctrl) {
    unsigned candidate = level;
    *direct_ctrl = 0;
    for (;;) {
        uint32_t value = active_keymap.symbols[candidate][keycode];
        if (value != TUNIX_KEYSYM_NONE) {
            *direct_ctrl = candidate == level &&
                           (candidate & TUNIX_KEYMAP_LEVEL_CTRL) != 0;
            return value;
        }
        if (candidate & TUNIX_KEYMAP_LEVEL_CTRL)
            candidate &= ~TUNIX_KEYMAP_LEVEL_CTRL;
        else if (candidate & TUNIX_KEYMAP_LEVEL_ALTGR)
            candidate &= ~TUNIX_KEYMAP_LEVEL_ALTGR;
        else if (candidate & TUNIX_KEYMAP_LEVEL_SHIFT)
            candidate &= ~TUNIX_KEYMAP_LEVEL_SHIFT;
        else
            return TUNIX_KEYSYM_NONE;
    }
}

static unsigned ansi_param(struct tty *tty, unsigned index, unsigned default_value) {
    if (index >= tty->ansi_param_count || tty->ansi_params[index] == 0)
        return default_value;
    return tty->ansi_params[index];
}

static void ansi_finish_param(struct tty *tty) {
    if (tty->ansi_param_count < ANSI_PARAM_MAX) {
        tty->ansi_params[tty->ansi_param_count++] =
            tty->ansi_have_current ? tty->ansi_current : 0;
    }
    tty->ansi_current = 0;
    tty->ansi_have_current = 0;
}

static void terminal_ansi_final(struct tty *tty, char command) {
    struct terminal_screen *screen = tty->screen;
    if (tty->ansi_have_current || tty->ansi_param_count == 0) ansi_finish_param(tty);
    switch (command) {
        case 'm':
            terminal_set_sgr_sequence(screen, tty->ansi_params, tty->ansi_param_count);
            break;
        case 'A': terminal_cursor_move(screen, -(int)ansi_param(tty, 0, 1), 0); break;
        case 'B': terminal_cursor_move(screen, (int)ansi_param(tty, 0, 1), 0); break;
        case 'C': terminal_cursor_move(screen, 0, (int)ansi_param(tty, 0, 1)); break;
        case 'D': terminal_cursor_move(screen, 0, -(int)ansi_param(tty, 0, 1)); break;
        case 'E': { int row, col; terminal_cursor_get(screen, &row, &col); (void)col; terminal_cursor_set(screen, row + (int)ansi_param(tty, 0, 1), 0); break; }
        case 'F': { int row, col; terminal_cursor_get(screen, &row, &col); (void)col; terminal_cursor_set(screen, row - (int)ansi_param(tty, 0, 1), 0); break; }
        case 'G': { int row, col; terminal_cursor_get(screen, &row, &col); (void)col; terminal_cursor_set(screen, row, (int)ansi_param(tty, 0, 1) - 1); break; }
        case 'H':
        case 'f': terminal_cursor_set(screen, (int)ansi_param(tty, 0, 1) - 1,
                                      (int)ansi_param(tty, 1, 1) - 1); break;
        case 'J': terminal_erase_display(screen, ansi_param(tty, 0, 0)); break;
        case 'K': terminal_erase_line(screen, ansi_param(tty, 0, 0)); break;
        case 'L': terminal_insert_lines(screen, ansi_param(tty, 0, 1)); break;
        case 'M': terminal_delete_lines(screen, ansi_param(tty, 0, 1)); break;
        case '@': terminal_insert_chars(screen, ansi_param(tty, 0, 1)); break;
        case 'P': terminal_delete_chars(screen, ansi_param(tty, 0, 1)); break;
        case 'X': terminal_erase_chars(screen, ansi_param(tty, 0, 1)); break;
        case 'S': terminal_scroll_up(screen, ansi_param(tty, 0, 1)); break;
        case 'T': terminal_scroll_down(screen, ansi_param(tty, 0, 1)); break;
        case 'r': terminal_set_scroll_region(screen, (int)ansi_param(tty, 0, 1),
                                             (int)ansi_param(tty, 1, 0)); break;
        case 's': terminal_cursor_get(screen, &tty->saved_row, &tty->saved_col); break;
        case 'u': terminal_cursor_set(screen, tty->saved_row, tty->saved_col); break;
        case 'a': terminal_cursor_move(screen, 0, (int)ansi_param(tty, 0, 1)); break;
        case 'd': { int row, col; terminal_cursor_get(screen, &row, &col); (void)row; terminal_cursor_set(screen, (int)ansi_param(tty, 0, 1) - 1, col); break; }
        case 'e': terminal_cursor_move(screen, (int)ansi_param(tty, 0, 1), 0); break;
        case 'h':
        case 'l':
            if (tty->ansi_private) {
                int enabled = command == 'h';
                for (unsigned i = 0; i < tty->ansi_param_count; i++) {
                    if (tty->ansi_params[i] == 25U)
                        terminal_set_cursor_visible(screen, enabled);
                    else if (tty->ansi_params[i] == 47U || tty->ansi_params[i] == 1047U ||
                             tty->ansi_params[i] == 1049U)
                        terminal_set_alternate_screen(screen, enabled);
                }
            }
            break;
        default: break;
    }
    tty->ansi_state = 0;
    tty->ansi_param_count = 0;
    tty->ansi_current = 0;
    tty->ansi_have_current = 0;
    tty->ansi_private = 0;
}

static void ansi_begin_csi(struct tty *tty) {
    tty->ansi_state = 2;
    tty->ansi_param_count = 0;
    tty->ansi_current = 0;
    tty->ansi_have_current = 0;
    tty->ansi_private = 0;
}

static void utf8_reset(struct tty *tty) {
    tty->utf8_codepoint = 0;
    tty->utf8_minimum = 0;
    tty->utf8_remaining = 0;
}

static void utf8_replacement(struct tty *tty) {
    utf8_reset(tty);
    terminal_put_codepoint(tty->screen, UINT32_C(0xFFFD));
}

static void terminal_feed_text_byte(struct tty *tty, uint8_t byte) {
    if (tty->utf8_remaining) {
        if ((byte & 0xC0U) != 0x80U) {
            utf8_replacement(tty);
            terminal_feed_text_byte(tty, byte);
            return;
        }
        tty->utf8_codepoint = (tty->utf8_codepoint << 6) | (uint32_t)(byte & 0x3FU);
        tty->utf8_remaining--;
        if (!tty->utf8_remaining) {
            uint32_t codepoint = tty->utf8_codepoint;
            uint32_t minimum = tty->utf8_minimum;
            utf8_reset(tty);
            if (codepoint < minimum || codepoint > UINT32_C(0x10FFFF) ||
                (codepoint >= UINT32_C(0xD800) && codepoint <= UINT32_C(0xDFFF)))
                terminal_put_codepoint(tty->screen, UINT32_C(0xFFFD));
            else
                terminal_put_codepoint(tty->screen, codepoint);
        }
        return;
    }

    if (byte < 0x80U) {
        terminal_put_codepoint(tty->screen, byte);
    } else if (byte >= 0xC2U && byte <= 0xDFU) {
        tty->utf8_codepoint = byte & 0x1FU;
        tty->utf8_minimum = 0x80U;
        tty->utf8_remaining = 1;
    } else if (byte >= 0xE0U && byte <= 0xEFU) {
        tty->utf8_codepoint = byte & 0x0FU;
        tty->utf8_minimum = 0x800U;
        tty->utf8_remaining = 2;
    } else if (byte >= 0xF0U && byte <= 0xF4U) {
        tty->utf8_codepoint = byte & 0x07U;
        tty->utf8_minimum = UINT32_C(0x10000);
        tty->utf8_remaining = 3;
    } else {
        terminal_put_codepoint(tty->screen, UINT32_C(0xFFFD));
    }
}

static void terminal_feed(struct tty *tty, char c) {
    struct terminal_screen *screen = tty->screen;
    if (tty->ansi_state == 0) {
        uint8_t byte = (uint8_t)c;
        if (byte == 0x1BU) {
            if (tty->utf8_remaining) utf8_replacement(tty);
            tty->ansi_state = 1;
            return;
        }
        terminal_feed_text_byte(tty, byte);
        return;
    }
    if (tty->ansi_state == 1) {
        if (c == '[') {
            ansi_begin_csi(tty);
            return;
        }
        if (c == ']') {
            tty->ansi_state = 3;
            return;
        }
        if (c == '7') {
            terminal_cursor_get(screen, &tty->saved_row, &tty->saved_col);
            tty->ansi_state = 0;
            return;
        }
        if (c == '8') {
            terminal_cursor_set(screen, tty->saved_row, tty->saved_col);
            tty->ansi_state = 0;
            return;
        }
        if (c == 'c') {
            utf8_reset(tty);
            terminal_set_alternate_screen(screen, 0);
            terminal_set_cursor_visible(screen, 1);
            terminal_set_sgr(screen, 0);
            terminal_set_scroll_region(screen, 0, 0);
            terminal_clear(screen);
            tty->ansi_state = 0;
            return;
        }
        if (c == 'E') {
            terminal_put_char(screen, '\n');
            tty->ansi_state = 0;
            return;
        }
        if (c == 'D') {
            int row, col;
            terminal_cursor_get(screen, &row, &col);
            terminal_put_char(screen, '\n');
            terminal_cursor_set(screen, row + 1, col);
            tty->ansi_state = 0;
            return;
        }
        if (c == 'M') {
            terminal_cursor_move(screen, -1, 0);
            tty->ansi_state = 0;
            return;
        }
        if (c == '(' || c == ')' || c == '*' || c == '+' || c == '-' ||
            c == '.' || c == '/' || c == '#' || c == '%') {
            tty->ansi_state = 5;
            return;
        }
        tty->ansi_state = 0;
        return;
    }
    if (tty->ansi_state == 3) {
        if ((unsigned char)c == 0x07U) tty->ansi_state = 0;
        else if ((unsigned char)c == 0x1BU) tty->ansi_state = 4;
        return;
    }
    if (tty->ansi_state == 4) {
        tty->ansi_state = c == '\\' ? 0 : 3;
        return;
    }
    if (tty->ansi_state == 5) {
        tty->ansi_state = 0;
        return;
    }
    if (c >= '0' && c <= '9') {
        tty->ansi_current = tty->ansi_current * 10U + (unsigned)(c - '0');
        tty->ansi_have_current = 1;
        return;
    }
    if (c == ';' || c == ':') {
        ansi_finish_param(tty);
        return;
    }
    if (c == '?' && tty->ansi_param_count == 0 && !tty->ansi_have_current) {
        tty->ansi_private = 1;
        return;
    }
    if (c == '>') return;
    terminal_ansi_final(tty, c);
}

/*
 * Everything written to any terminal is mirrored to the serial line. On a
 * machine with several of them that interleaves, but the serial log is the only
 * record of a terminal nobody is looking at, and losing it would mean losing
 * the output of every service that is not on the terminal in front of you.
 */
static void emit_char(struct tty *tty, char c) {
    serial_write_char(c);
    terminal_feed(tty, c);
}

int64_t tty_write(struct tty *tty, size_t size, const void *buffer) {
    if (!tty || !buffer) return -1;
    const char *bytes = (const char *)buffer;
    for (size_t i = 0; i < size; i++) emit_char(tty, bytes[i]);
    return (int64_t)size;
}

static int signal_input_character(struct tty *tty, uint8_t value) {
    if (!(tty->termios.lflag & TTY_ISIG) || tty->foreground_pgid <= 0)
        return 0;

    int signal_number = 0;
    const char *echo = NULL;
    size_t echo_length = 0;
    if (value == tty->termios.cc[TTY_VINTR]) {
        signal_number = SIGINT;
        echo = "^C\n";
        echo_length = 3;
    } else if (value == tty->termios.cc[TTY_VQUIT]) {
        signal_number = SIGQUIT;
        echo = "^\\\n";
        echo_length = 3;
    } else if (value == tty->termios.cc[TTY_VSUSP]) {
        signal_number = SIGTSTP;
        echo = "^Z\n";
        echo_length = 3;
    } else {
        return 0;
    }

    tty->canonical_length = tty->canonical_offset = 0;
    tty->input_head = tty->input_tail = tty->input_count = 0;
    tty->input_interrupted = 1;
    if ((tty->termios.lflag & TTY_ECHO) && echo)
        (void)tty_write(tty, echo_length, echo);
    (void)process_send_signal(-(int64_t)tty->foreground_pgid, signal_number);
    return 1;
}

static int input_push(struct tty *tty, uint8_t value) {
    /* Either way something has happened that a blocked reader cares about: a
       character to take, or a signal that ends its read. Waking is safe from
       the keyboard interrupt because everything here runs under the kernel
       lock with interrupts off, so no reader can be between deciding it has
       nothing to do and going to sleep. */
    if (signal_input_character(tty, value)) {
        vt_input_arrived();
        return 0;
    }
    if (tty->input_count == TTY_INPUT_CAPACITY) return -1;
    tty->input_buffer[tty->input_tail] = value;
    tty->input_tail = (tty->input_tail + 1U) % TTY_INPUT_CAPACITY;
    tty->input_count++;
    vt_input_arrived();
    return 0;
}

static void input_push_text(struct tty *tty, const char *text) {
    while (*text) {
        if (input_push(tty, (uint8_t)*text++) != 0) break;
    }
}

static int input_pop(struct tty *tty) {
    if (tty->input_count == 0) return -1;
    int value = tty->input_buffer[tty->input_head];
    tty->input_head = (tty->input_head + 1U) % TTY_INPUT_CAPACITY;
    tty->input_count--;
    return value;
}

void tty_push_serial(struct tty *tty, uint8_t value) {
    if (tty) (void)input_push(tty, value);
}

void tty_flush_input(struct tty *tty) {
    if (!tty) return;
    tty->canonical_length = tty->canonical_offset = 0;
    tty->input_head = tty->input_tail = tty->input_count = 0;
}

void tty_reset_keyboard_state(void) {
    shift_down = 0;
    ctrl_down = 0;
    alt_down = 0;
    altgr_down = 0;
}

/* The keys that stand for a sequence rather than a character. Terminfo calls
   these kcuu1, kend, kf1 and so on; they are what makes an arrow key move the
   cursor in a shell instead of doing nothing. */
static const char *key_sequence(uint16_t keycode) {
    switch (keycode) {
        case TUNIX_KEY_UP: return "\x1b[A";
        case TUNIX_KEY_DOWN: return "\x1b[B";
        case TUNIX_KEY_RIGHT: return "\x1b[C";
        case TUNIX_KEY_LEFT: return "\x1b[D";
        case TUNIX_KEY_HOME: return "\x1b[H";
        case TUNIX_KEY_END: return "\x1b[F";
        case TUNIX_KEY_INSERT: return "\x1b[2~";
        case TUNIX_KEY_DELETE: return "\x1b[3~";
        case TUNIX_KEY_PAGEUP: return "\x1b[5~";
        case TUNIX_KEY_PAGEDOWN: return "\x1b[6~";
        case TUNIX_KEY_F1: return "\x1bOP";
        case TUNIX_KEY_F2: return "\x1bOQ";
        case TUNIX_KEY_F3: return "\x1bOR";
        case TUNIX_KEY_F4: return "\x1bOS";
        case TUNIX_KEY_F5: return "\x1b[15~";
        case TUNIX_KEY_F6: return "\x1b[17~";
        case TUNIX_KEY_F7: return "\x1b[18~";
        case TUNIX_KEY_F8: return "\x1b[19~";
        case TUNIX_KEY_F9: return "\x1b[20~";
        case TUNIX_KEY_F10: return "\x1b[21~";
        case TUNIX_KEY_F11: return "\x1b[23~";
        case TUNIX_KEY_F12: return "\x1b[24~";
        default: return NULL;
    }
}

/*
 * One key, as a keycode rather than as a scancode.
 *
 * Keycodes are what the keymap has always been indexed by -- loadkeys(1) reads
 * "keycode N = symbol" out of a keymap file, the same as on Linux -- and it
 * only worked when fed scancodes because the two coincide for the main block
 * of a set-1 keyboard. Speaking keycodes here is both the correction of that
 * and the reason a USB keyboard now reaches the console at all: the PS/2
 * driver decodes scancodes into keycodes already, and the HID driver produces
 * nothing else, so the two meet here instead of only in evdev.
 */
void tty_handle_key(struct tty *tty, uint16_t keycode, int pressed) {
    if (!tty) return;
    switch (keycode) {
        case TUNIX_KEY_LEFTSHIFT:
        case TUNIX_KEY_RIGHTSHIFT: shift_down = pressed; return;
        case TUNIX_KEY_LEFTCTRL:
        case TUNIX_KEY_RIGHTCTRL: ctrl_down = pressed; return;
        case TUNIX_KEY_LEFTALT: alt_down = pressed; return;
        case TUNIX_KEY_RIGHTALT: altgr_down = pressed; return;
        /* Caps lock turns over on the press and is left alone on the release,
           or holding it down would turn it over twice. */
        case TUNIX_KEY_CAPSLOCK:
            if (pressed) caps_lock = !caps_lock;
            return;
        default: break;
    }
    /* A release changes nothing else: the character was delivered when the key
       went down, and a held key repeats through another press. */
    if (!pressed) return;

    if (keycode == TUNIX_KEY_ESC) { (void)input_push(tty, 0x1BU); return; }
    if (keycode == TUNIX_KEY_BACKSPACE) { (void)input_push(tty, 127U); return; }
    if (keycode == TUNIX_KEY_TAB && shift_down) {
        input_push_text(tty, "\x1b[Z");
        return;
    }
    if (keycode == TUNIX_KEY_KPENTER) { (void)input_push(tty, '\r'); return; }
    if (keycode == TUNIX_KEY_KPSLASH) { (void)input_push(tty, '/'); return; }
    const char *sequence = key_sequence(keycode);
    if (sequence) {
        input_push_text(tty, sequence);
        return;
    }
    if (keycode >= TUNIX_KEYMAP_KEYCODES) return;

    unsigned level = (shift_down ? TUNIX_KEYMAP_LEVEL_SHIFT : 0U) |
                     (altgr_down ? TUNIX_KEYMAP_LEVEL_ALTGR : 0U) |
                     (ctrl_down ? TUNIX_KEYMAP_LEVEL_CTRL : 0U);
    if (caps_lock && keymap_is_letter(keycode)) level ^= TUNIX_KEYMAP_LEVEL_SHIFT;
    int direct_ctrl = 0;
    uint32_t value = keymap_lookup(keycode, level, &direct_ctrl);
    if (value == TUNIX_KEYSYM_NONE) return;
    if (ctrl_down && !direct_ctrl) {
        if (value == ' ') value = 0;
        else if (value >= 'a' && value <= 'z') value = value - 'a' + 1U;
        else if (value >= 'A' && value <= 'Z') value = value - 'A' + 1U;
        else if (value >= '@' && value <= '_') value &= 0x1FU;
        else if (value == '?') value = 0x7FU;
    }
    if (alt_down && input_push(tty, 0x1BU) != 0) return;
    (void)input_push_codepoint(tty, value);
}

/*
 * Take one character from the queue.
 *
 * Nothing waits here. A terminal only ever has characters put in it by the
 * keyboard or the serial line, and both do that from an interrupt; the read
 * path is only entered once tty_input_ready() has said there is something to
 * take, and a terminal that is not the active one is never given anything at
 * all. Spinning would hold the kernel lock against the very interrupt that
 * would end the spin.
 */
static int read_input_char(struct tty *tty) {
    vt_poll_input();
    if (tty->input_interrupted) {
        tty->input_interrupted = 0;
        return -EINTR;
    }
    int value = input_pop(tty);
    return value >= 0 ? value : -EAGAIN;
}

static int canonical_input_complete(struct tty *tty) {
    if (tty->canonical_offset < tty->canonical_length) return 1;
    for (size_t i = 0, at = tty->input_head; i < tty->input_count; i++) {
        uint8_t value = tty->input_buffer[at];
        if (value == '\n' || value == '\r' ||
            value == tty->termios.cc[TTY_VEOF]) return 1;
        at = (at + 1U) % TTY_INPUT_CAPACITY;
    }
    /* A queue that has filled without a delimiter has to be handed over as it
       is; the alternative is a terminal that never answers again. */
    return tty->input_count >= TTY_CANONICAL_CAPACITY;
}

int tty_input_ready(struct tty *tty) {
    if (!tty) return 0;
    vt_poll_input();
    if (tty->input_interrupted) return 1;
    if (!(tty->termios.lflag & TTY_ICANON)) return tty->input_count != 0;
    return canonical_input_complete(tty);
}

static int refill_canonical(struct tty *tty) {
    tty->canonical_length = 0;
    tty->canonical_offset = 0;
    int saw_end_of_file = 0;
    int starved = 0;
    while (tty->canonical_length < sizeof(tty->canonical_buffer)) {
        int value = read_input_char(tty);
        /* Nothing queued. Distinguished from end of file below: a terminal with
           an empty queue has not ended, it simply has nothing typed at it yet,
           and answering 0 there would look like Ctrl-D to every shell. */
        if (value == -EAGAIN) {
            starved = 1;
            break;
        }
        if (value < 0) return value;
        if (value == tty->termios.cc[TTY_VEOF]) {
            saw_end_of_file = 1;
            break;
        }
        if (value == '\r') value = '\n';
        if (value == tty->termios.cc[TTY_VERASE] || value == '\b' || value == 127) {
            if (tty->canonical_length) {
                tty->canonical_length--;
                if (tty->termios.lflag & TTY_ECHO) tty_write(tty, 3, "\b \b");
            }
            continue;
        }
        tty->canonical_buffer[tty->canonical_length++] = (char)value;
        if (tty->termios.lflag & TTY_ECHO) {
            char c = (char)value;
            tty_write(tty, 1, &c);
        }
        if (value == '\n') break;
    }
    if (tty->canonical_length == 0) {
        if (saw_end_of_file) return 0;
        if (starved) return -EAGAIN;
    }
    return (int)tty->canonical_length;
}

int64_t tty_read(struct tty *tty, size_t size, void *buffer) {
    if (!tty || !buffer || size == 0) return 0;
    struct process *reader = process_current();
    if (reader && tty->session > 0 && reader->sid == tty->session &&
        tty->foreground_pgid > 0 &&
        reader->pgid != (uint64_t)tty->foreground_pgid) {
        (void)process_send_signal(-(int64_t)reader->pgid, SIGTTIN);
        return -EINTR;
    }
    char *out = (char *)buffer;

    if (!(tty->termios.lflag & TTY_ICANON)) {
        size_t received = 0;
        int value = read_input_char(tty);
        if (value < 0) return value;
        out[received++] = (char)value;
        if (tty->termios.lflag & TTY_ECHO) {
            char c = (char)value;
            tty_write(tty, 1, &c);
        }
        while (received < size && tty->input_count) out[received++] = (char)input_pop(tty);
        return (int64_t)received;
    }

    if (tty->canonical_offset >= tty->canonical_length) {
        int result = refill_canonical(tty);
        if (result <= 0) return result;
    }
    size_t available = tty->canonical_length - tty->canonical_offset;
    if (size > available) size = available;
    memcpy(out, tty->canonical_buffer + tty->canonical_offset, size);
    tty->canonical_offset += size;
    return (int64_t)size;
}

struct tty *tty_create(struct terminal_screen *screen) {
    struct tty *tty = kmalloc(sizeof(*tty));
    if (!tty) return NULL;
    memset(tty, 0, sizeof(*tty));
    tty->screen = screen;
    tty->termios.iflag = 0x00000500U;
    tty->termios.oflag = 0x00000005U;
    tty->termios.cflag = 0x000000BFU;
    tty->termios.lflag = TTY_ECHO | TTY_ECHOE | TTY_ECHOK |
                         TTY_ICANON | TTY_ISIG | TTY_IEXTEN;
    tty->termios.cc[TTY_VINTR] = 3;
    tty->termios.cc[TTY_VQUIT] = 28;
    tty->termios.cc[TTY_VERASE] = 127;
    tty->termios.cc[TTY_VKILL] = 21;
    tty->termios.cc[TTY_VEOF] = 4;
    tty->termios.cc[TTY_VTIME] = 0;
    tty->termios.cc[TTY_VMIN] = 1;
    tty->termios.cc[TTY_VSTART] = 17;
    tty->termios.cc[TTY_VSTOP] = 19;
    tty->termios.cc[TTY_VSUSP] = 26;
    tty->termios.ispeed = 38400;
    tty->termios.ospeed = 38400;
    if (!keymap_loaded) {
        keymap_load_default();
        tty_reset_keyboard_state();
        caps_lock = 0;
    }
    return tty;
}

void tty_destroy(struct tty *tty) {
    kfree(tty);
}

struct terminal_screen *tty_screen(const struct tty *tty) {
    return tty ? tty->screen : NULL;
}

int tty_ioctl(struct tty *tty, unsigned long request, void *argument) {
    if (!tty || !argument) return -1;
    switch (request) {
        case TCGETS:
            memcpy(argument, &tty->termios, sizeof(tty->termios));
            return 0;
        case TCSETS:
        case TCSETSW:
        case TCSETSF:
            memcpy(&tty->termios, argument, sizeof(tty->termios));
            if (request == TCSETSF) tty_flush_input(tty);
            return 0;
        case TIOCGPGRP:
            *(int *)argument = tty->foreground_pgid;
            return 0;
        case TIOCSPGRP:
            tty->foreground_pgid = *(const int *)argument;
            tty->input_interrupted = 0;
            return 0;
        case TIOCGETD:
            *(int *)argument = 0;
            return 0;
        case TIOCSETD:
            return *(const int *)argument == 0 ? 0 : -1;
        case TUNIX_KDGKBMAP:
            memcpy(argument, &active_keymap, sizeof(active_keymap));
            return 0;
        case TUNIX_KDSKBMAP:
            if (keymap_validate((const struct tunix_keymap *)argument) != 0) return -1;
            memcpy(&active_keymap, argument, sizeof(active_keymap));
            tty_reset_keyboard_state();
            return 0;
        case KDGKBTYPE:
            *(uint8_t *)argument = TUNIX_KB_101;
            return 0;
        default:
            return -1;
    }
}

int tty_foreground_pgid(const struct tty *tty) {
    return tty ? tty->foreground_pgid : 0;
}

void tty_set_foreground_pgid(struct tty *tty, int pgid) {
    if (!tty) return;
    tty->foreground_pgid = pgid;
    tty->input_interrupted = 0;
}

uint64_t tty_session(const struct tty *tty) { return tty ? tty->session : 0; }

void tty_set_controlling_session(struct tty *tty, uint64_t sid, int pgid) {
    if (!tty) return;
    tty->session = sid;
    tty->foreground_pgid = pgid;
    tty->input_interrupted = 0;
}

/* TIOCNOTTY. The foreground group is left alone: it is what Ctrl-C is aimed
   at, and nothing takes over as the terminal's session until a login does. */
void tty_release_controlling_session(struct tty *tty, uint64_t sid) {
    if (!tty) return;
    if (tty->session && tty->session != sid) return;
    tty->session = 0;
}
