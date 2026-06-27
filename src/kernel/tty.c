#include <stddef.h>
#include <stdint.h>
#include "include/io.h"
#include "include/input.h"
#include "include/kstring.h"
#include "include/process.h"
#include "include/signal.h"
#include "include/tty.h"
#include "include/terminal.h"

#define EINTR 4
#define TTY_INPUT_CAPACITY 1024
#define ANSI_PARAM_MAX 16

extern void serial_write_char(char c);

static struct tunix_termios console_termios;
static int console_foreground_pgid;
static char canonical_buffer[1024];
static size_t canonical_length;
static size_t canonical_offset;
static uint8_t input_buffer[TTY_INPUT_CAPACITY];
static size_t input_head;
static size_t input_tail;
static size_t input_count;
static int shift_down;
static int ctrl_down;
static int alt_down;
static int caps_lock;
static int extended_prefix;

static int ansi_state;
static unsigned ansi_params[ANSI_PARAM_MAX];
static unsigned ansi_param_count;
static unsigned ansi_current;
static int ansi_have_current;
static int ansi_private;
static int saved_row;
static int saved_col;

static const char keymap[128] = {
    [0x02]='1',[0x03]='2',[0x04]='3',[0x05]='4',[0x06]='5',[0x07]='6',[0x08]='7',[0x09]='8',[0x0A]='9',[0x0B]='0',
    [0x0C]='-',[0x0D]='=',[0x0E]='\b',[0x0F]='\t',[0x10]='q',[0x11]='w',[0x12]='e',[0x13]='r',[0x14]='t',[0x15]='y',
    [0x16]='u',[0x17]='i',[0x18]='o',[0x19]='p',[0x1A]='[',[0x1B]=']',[0x1C]='\n',[0x1E]='a',[0x1F]='s',[0x20]='d',
    [0x21]='f',[0x22]='g',[0x23]='h',[0x24]='j',[0x25]='k',[0x26]='l',[0x27]=';',[0x28]='\'',[0x29]='`',[0x2B]='\\',
    [0x2C]='z',[0x2D]='x',[0x2E]='c',[0x2F]='v',[0x30]='b',[0x31]='n',[0x32]='m',[0x33]=',',[0x34]='.',[0x35]='/',[0x39]=' '
};

static const char shift_keymap[128] = {
    [0x02]='!',[0x03]='@',[0x04]='#',[0x05]='$',[0x06]='%',[0x07]='^',[0x08]='&',[0x09]='*',[0x0A]='(',[0x0B]=')',
    [0x0C]='_',[0x0D]='+',[0x10]='Q',[0x11]='W',[0x12]='E',[0x13]='R',[0x14]='T',[0x15]='Y',[0x16]='U',[0x17]='I',
    [0x18]='O',[0x19]='P',[0x1A]='{',[0x1B]='}',[0x1E]='A',[0x1F]='S',[0x20]='D',[0x21]='F',[0x22]='G',[0x23]='H',
    [0x24]='J',[0x25]='K',[0x26]='L',[0x27]=':',[0x28]='"',[0x29]='~',[0x2B]='|',[0x2C]='Z',[0x2D]='X',[0x2E]='C',
    [0x2F]='V',[0x30]='B',[0x31]='N',[0x32]='M',[0x33]='<',[0x34]='>',[0x35]='?',[0x39]=' '
};

static unsigned ansi_param(unsigned index, unsigned default_value) {
    if (index >= ansi_param_count || ansi_params[index] == 0) return default_value;
    return ansi_params[index];
}

static void ansi_finish_param(void) {
    if (ansi_param_count < ANSI_PARAM_MAX) {
        ansi_params[ansi_param_count++] = ansi_have_current ? ansi_current : 0;
    }
    ansi_current = 0;
    ansi_have_current = 0;
}

static void terminal_ansi_final(char command) {
    if (ansi_have_current || ansi_param_count == 0) ansi_finish_param();
    switch (command) {
        case 'm':
            terminal_set_sgr_sequence(ansi_params, ansi_param_count);
            break;
        case 'A': terminal_cursor_move(-(int)ansi_param(0, 1), 0); break;
        case 'B': terminal_cursor_move((int)ansi_param(0, 1), 0); break;
        case 'C': terminal_cursor_move(0, (int)ansi_param(0, 1)); break;
        case 'D': terminal_cursor_move(0, -(int)ansi_param(0, 1)); break;
        case 'E': { int row, col; terminal_cursor_get(&row, &col); (void)col; terminal_cursor_set(row + (int)ansi_param(0, 1), 0); break; }
        case 'F': { int row, col; terminal_cursor_get(&row, &col); (void)col; terminal_cursor_set(row - (int)ansi_param(0, 1), 0); break; }
        case 'G': { int row, col; terminal_cursor_get(&row, &col); (void)col; terminal_cursor_set(row, (int)ansi_param(0, 1) - 1); break; }
        case 'H':
        case 'f': terminal_cursor_set((int)ansi_param(0, 1) - 1, (int)ansi_param(1, 1) - 1); break;
        case 'J': terminal_erase_display(ansi_param(0, 0)); break;
        case 'K': terminal_erase_line(ansi_param(0, 0)); break;
        case 'L': terminal_insert_lines(ansi_param(0, 1)); break;
        case 'M': terminal_delete_lines(ansi_param(0, 1)); break;
        case '@': terminal_insert_chars(ansi_param(0, 1)); break;
        case 'P': terminal_delete_chars(ansi_param(0, 1)); break;
        case 'X': terminal_erase_chars(ansi_param(0, 1)); break;
        case 'S': terminal_scroll_up(ansi_param(0, 1)); break;
        case 'T': terminal_scroll_down(ansi_param(0, 1)); break;
        case 'r': terminal_set_scroll_region((int)ansi_param(0, 1),
                                             (int)ansi_param(1, 0)); break;
        case 's': terminal_cursor_get(&saved_row, &saved_col); break;
        case 'u': terminal_cursor_set(saved_row, saved_col); break;
        case 'a': terminal_cursor_move(0, (int)ansi_param(0, 1)); break;
        case 'd': { int row, col; terminal_cursor_get(&row, &col); (void)row; terminal_cursor_set((int)ansi_param(0, 1) - 1, col); break; }
        case 'e': terminal_cursor_move((int)ansi_param(0, 1), 0); break;
        case 'h':
        case 'l':
            if (ansi_private) {
                int enabled = command == 'h';
                for (unsigned i = 0; i < ansi_param_count; i++) {
                    if (ansi_params[i] == 25U) terminal_set_cursor_visible(enabled);
                    else if (ansi_params[i] == 47U || ansi_params[i] == 1047U ||
                             ansi_params[i] == 1049U) terminal_set_alternate_screen(enabled);
                }
            }
            break;
        default: break;
    }
    ansi_state = 0;
    ansi_param_count = 0;
    ansi_current = 0;
    ansi_have_current = 0;
    ansi_private = 0;
}

static void ansi_begin_csi(void) {
    ansi_state = 2;
    ansi_param_count = 0;
    ansi_current = 0;
    ansi_have_current = 0;
    ansi_private = 0;
}

static void terminal_feed(char c) {
    if (ansi_state == 0) {
        if ((unsigned char)c == 0x1BU) {
            ansi_state = 1;
            return;
        }
        terminal_put_char(c);
        return;
    }
    if (ansi_state == 1) {
        if (c == '[') {
            ansi_begin_csi();
            return;
        }
        if (c == ']') {
            ansi_state = 3;
            return;
        }
        if (c == '7') {
            terminal_cursor_get(&saved_row, &saved_col);
            ansi_state = 0;
            return;
        }
        if (c == '8') {
            terminal_cursor_set(saved_row, saved_col);
            ansi_state = 0;
            return;
        }
        if (c == 'c') {
            terminal_set_alternate_screen(0);
            terminal_set_cursor_visible(1);
            terminal_set_sgr(0);
            terminal_set_scroll_region(0, 0);
            terminal_clear();
            ansi_state = 0;
            return;
        }
        if (c == 'E') {
            terminal_put_char('\n');
            ansi_state = 0;
            return;
        }
        if (c == 'D') {
            int row, col;
            terminal_cursor_get(&row, &col);
            (void)row;
            terminal_put_char('\n');
            terminal_cursor_set(row + 1, col);
            ansi_state = 0;
            return;
        }
        if (c == 'M') {
            terminal_cursor_move(-1, 0);
            ansi_state = 0;
            return;
        }
        if (c == '(' || c == ')' || c == '*' || c == '+' || c == '-' ||
            c == '.' || c == '/' || c == '#' || c == '%') {
            ansi_state = 5;
            return;
        }
        ansi_state = 0;
        return;
    }
    if (ansi_state == 3) {
        if ((unsigned char)c == 0x07U) ansi_state = 0;
        else if ((unsigned char)c == 0x1BU) ansi_state = 4;
        return;
    }
    if (ansi_state == 4) {
        ansi_state = c == '\\' ? 0 : 3;
        return;
    }
    if (ansi_state == 5) {
        ansi_state = 0;
        return;
    }
    if (c >= '0' && c <= '9') {
        ansi_current = ansi_current * 10U + (unsigned)(c - '0');
        ansi_have_current = 1;
        return;
    }
    if (c == ';' || c == ':') {
        ansi_finish_param();
        return;
    }
    if (c == '?' && ansi_param_count == 0 && !ansi_have_current) {
        ansi_private = 1;
        return;
    }
    if (c == '>') return;
    terminal_ansi_final(c);
}

static void emit_char(char c) {
    serial_write_char(c);
    terminal_feed(c);
}

int64_t tty_write(size_t size, const void *buffer) {
    if (!buffer) return -1;
    const char *bytes = (const char *)buffer;
    for (size_t i = 0; i < size; i++) emit_char(bytes[i]);
    return (int64_t)size;
}

static int input_push(uint8_t value) {
    if (input_count == TTY_INPUT_CAPACITY) return -1;
    input_buffer[input_tail] = value;
    input_tail = (input_tail + 1U) % TTY_INPUT_CAPACITY;
    input_count++;
    return 0;
}

static void input_push_text(const char *text) {
    while (*text) {
        if (input_push((uint8_t)*text++) != 0) break;
    }
}

static int input_pop(void) {
    if (input_count == 0) return -1;
    int value = input_buffer[input_head];
    input_head = (input_head + 1U) % TTY_INPUT_CAPACITY;
    input_count--;
    return value;
}

static int serial_try_read(void) {
    if (inb(0x3FD) & 1U) return inb(0x3F8);
    return -1;
}

static int is_letter(char value) {
    return value >= 'a' && value <= 'z';
}

void tty_reset_keyboard_state(void) {
    shift_down = 0;
    ctrl_down = 0;
    alt_down = 0;
    extended_prefix = 0;
}

void tty_handle_scancode(uint8_t code) {
    if (code == 0xE0U) {
        extended_prefix = 1;
        return;
    }
    if (code == 0xE1U) {
        extended_prefix = 0;
        return;
    }
    if (extended_prefix) {
        extended_prefix = 0;
        uint8_t released = code & 0x80U;
        code &= 0x7FU;
        if (code == 0x1DU) {
            ctrl_down = released ? 0 : 1;
            return;
        }
        if (code == 0x38U) {
            alt_down = released ? 0 : 1;
            return;
        }
        if (released) return;
        switch (code) {
            case 0x48U: input_push_text("\x1b[A"); return;
            case 0x50U: input_push_text("\x1b[B"); return;
            case 0x4DU: input_push_text("\x1b[C"); return;
            case 0x4BU: input_push_text("\x1b[D"); return;
            case 0x47U: input_push_text("\x1b[H"); return;
            case 0x4FU: input_push_text("\x1b[F"); return;
            case 0x52U: input_push_text("\x1b[2~"); return;
            case 0x53U: input_push_text("\x1b[3~"); return;
            case 0x49U: input_push_text("\x1b[5~"); return;
            case 0x51U: input_push_text("\x1b[6~"); return;
            default: return;
        }
    }
    if (code == 0x2AU || code == 0x36U) { shift_down = 1; return; }
    if (code == 0xAAU || code == 0xB6U) { shift_down = 0; return; }
    if (code == 0x1DU) { ctrl_down = 1; return; }
    if (code == 0x9DU) { ctrl_down = 0; return; }
    if (code == 0x38U) { alt_down = 1; return; }
    if (code == 0xB8U) { alt_down = 0; return; }
    if (code == 0x3AU) { caps_lock = !caps_lock; return; }
    if (code == 0x01U) { (void)input_push(0x1BU); return; }
    if (code & 0x80U || code >= 128U) return;
    if (code == 0x0EU) { (void)input_push(127U); return; }
    if (code == 0x0FU && shift_down) { input_push_text("\x1b[Z"); return; }
    switch (code) {
        case 0x3BU: input_push_text("\x1bOP"); return;
        case 0x3CU: input_push_text("\x1bOQ"); return;
        case 0x3DU: input_push_text("\x1bOR"); return;
        case 0x3EU: input_push_text("\x1bOS"); return;
        case 0x3FU: input_push_text("\x1b[15~"); return;
        case 0x40U: input_push_text("\x1b[17~"); return;
        case 0x41U: input_push_text("\x1b[18~"); return;
        case 0x42U: input_push_text("\x1b[19~"); return;
        case 0x43U: input_push_text("\x1b[20~"); return;
        case 0x44U: input_push_text("\x1b[21~"); return;
        case 0x57U: input_push_text("\x1b[23~"); return;
        case 0x58U: input_push_text("\x1b[24~"); return;
        default: break;
    }
    char value = shift_down ? shift_keymap[code] : keymap[code];
    if (!value) return;
    if (is_letter(keymap[code]) && caps_lock)
        value = shift_down ? keymap[code] : shift_keymap[code];
    if (ctrl_down && value >= 'a' && value <= 'z') value = (char)(value - 'a' + 1);
    else if (ctrl_down && value >= 'A' && value <= 'Z') value = (char)(value - 'A' + 1);
    else if (ctrl_down && value >= '@' && value <= '_') value &= 0x1F;
    if (alt_down && input_push(0x1BU) != 0) return;
    (void)input_push((uint8_t)value);
}

void tty_poll_inputs(void) {
    for (;;) {
        int value = serial_try_read();
        if (value < 0) break;
        if (input_push((uint8_t)value) != 0) break;
    }
    input_poll();
}

static int read_input_char(void) {
    for (;;) {
        tty_poll_inputs();
        int value = input_pop();
        if (value >= 0) return value;
        __asm__ volatile("pause");
    }
}

static int canonical_input_complete(void) {
    if (canonical_offset < canonical_length) return 1;
    for (size_t i = 0, at = input_head; i < input_count; i++) {
        uint8_t value = input_buffer[at];
        if (value == '\n' || value == '\r' ||
            value == console_termios.cc[TTY_VEOF] ||
            value == console_termios.cc[TTY_VINTR]) return 1;
        at = (at + 1U) % TTY_INPUT_CAPACITY;
    }
    return 0;
}

int tty_input_ready(void) {
    tty_poll_inputs();
    if (!(console_termios.lflag & TTY_ICANON)) return input_count != 0;
    return canonical_input_complete();
}

static int refill_canonical(void) {
    canonical_length = 0;
    canonical_offset = 0;
    while (canonical_length < sizeof(canonical_buffer)) {
        int value = read_input_char();
        if (value == console_termios.cc[TTY_VINTR] && (console_termios.lflag & TTY_ISIG)) {
            if (console_termios.lflag & TTY_ECHO) tty_write(2, "^C");
            tty_write(1, "\n");
            if (console_foreground_pgid > 0)
                process_send_signal(-(int64_t)console_foreground_pgid, SIGINT);
            return -EINTR;
        }
        if (value == console_termios.cc[TTY_VEOF]) {
            if (canonical_length == 0) return 0;
            break;
        }
        if (value == '\r') value = '\n';
        if (value == console_termios.cc[TTY_VERASE] || value == '\b' || value == 127) {
            if (canonical_length) {
                canonical_length--;
                if (console_termios.lflag & TTY_ECHO) tty_write(3, "\b \b");
            }
            continue;
        }
        canonical_buffer[canonical_length++] = (char)value;
        if (console_termios.lflag & TTY_ECHO) {
            char c = (char)value;
            tty_write(1, &c);
        }
        if (value == '\n') break;
    }
    return (int)canonical_length;
}

int64_t tty_read(size_t size, void *buffer) {
    if (!buffer || size == 0) return 0;
    char *out = (char *)buffer;

    if (!(console_termios.lflag & TTY_ICANON)) {
        size_t received = 0;
        int value = read_input_char();
        out[received++] = (char)value;
        if (console_termios.lflag & TTY_ECHO) {
            char c = (char)value;
            tty_write(1, &c);
        }
        tty_poll_inputs();
        while (received < size && input_count) out[received++] = (char)input_pop();
        return (int64_t)received;
    }

    if (canonical_offset >= canonical_length) {
        int result = refill_canonical();
        if (result <= 0) return result;
    }
    size_t available = canonical_length - canonical_offset;
    if (size > available) size = available;
    memcpy(out, canonical_buffer + canonical_offset, size);
    canonical_offset += size;
    return (int64_t)size;
}

void tty_init(void) {
    memset(&console_termios, 0, sizeof(console_termios));
    console_termios.iflag = 0x00000500U;
    console_termios.oflag = 0x00000005U;
    console_termios.cflag = 0x000000BFU;
    console_termios.lflag = TTY_ECHO | TTY_ECHOE | TTY_ECHOK |
                            TTY_ICANON | TTY_ISIG | TTY_IEXTEN;
    console_termios.cc[TTY_VINTR] = 3;
    console_termios.cc[TTY_VQUIT] = 28;
    console_termios.cc[TTY_VERASE] = 127;
    console_termios.cc[TTY_VKILL] = 21;
    console_termios.cc[TTY_VEOF] = 4;
    console_termios.cc[TTY_VTIME] = 0;
    console_termios.cc[TTY_VMIN] = 1;
    console_termios.cc[TTY_VSTART] = 17;
    console_termios.cc[TTY_VSTOP] = 19;
    console_termios.cc[TTY_VSUSP] = 26;
    console_termios.ispeed = 38400;
    console_termios.ospeed = 38400;
    console_foreground_pgid = 0;
    canonical_length = canonical_offset = 0;
    input_head = input_tail = input_count = 0;
    shift_down = ctrl_down = alt_down = caps_lock = extended_prefix = 0;
    ansi_state = 0;
    ansi_param_count = ansi_current = 0;
    ansi_have_current = 0;
    saved_row = saved_col = 0;
}

int tty_ioctl(unsigned long request, void *argument) {
    if (!argument) return -1;
    switch (request) {
        case TCGETS:
            memcpy(argument, &console_termios, sizeof(console_termios));
            return 0;
        case TCSETS:
        case TCSETSW:
        case TCSETSF:
            memcpy(&console_termios, argument, sizeof(console_termios));
            if (request == TCSETSF) {
                canonical_length = canonical_offset = 0;
                input_head = input_tail = input_count = 0;
            }
            return 0;
        case TIOCGPGRP:
            *(int *)argument = console_foreground_pgid;
            return 0;
        case TIOCSPGRP:
            console_foreground_pgid = *(const int *)argument;
            return 0;
        case TIOCGETD:
            *(int *)argument = 0;
            return 0;
        case TIOCSETD:
            return *(const int *)argument == 0 ? 0 : -1;
        default:
            return -1;
    }
}

int tty_foreground_pgid(void) { return console_foreground_pgid; }
void tty_set_foreground_pgid(int pgid) { console_foreground_pgid = pgid; }
