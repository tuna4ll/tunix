#ifndef TUNIX_TTY_H
#define TUNIX_TTY_H

#include <stddef.h>
#include <stdint.h>
#include <tunix/keymap.h>

#define TCGETS      0x5401UL
#define TCSETS      0x5402UL
#define TCSETSW     0x5403UL
#define TCSETSF     0x5404UL
#define TIOCSCTTY   0x540EUL
#define TIOCNOTTY   0x5422UL
#define TIOCGPGRP   0x540FUL
#define TIOCSPGRP   0x5410UL
#define TIOCGWINSZ  0x5413UL
#define TIOCGETD    0x5424UL
#define TIOCSETD    0x5423UL

/* The console/keyboard and virtual-terminal ioctls. Tunix has real virtual
   terminals -- see vt.c -- so these are answered for the terminal the caller
   has open rather than fixed at one. */
#define KDGKBTYPE     0x4B33UL
#define KDSETMODE     0x4B3AUL
#define KDGETMODE     0x4B3BUL
#define KDGKBMODE     0x4B44UL
#define KDSKBMODE     0x4B45UL
#define VT_OPENQRY    0x5600UL
#define VT_GETMODE    0x5601UL
#define VT_SETMODE    0x5602UL
#define VT_GETSTATE   0x5603UL
#define VT_RELDISP    0x5605UL
#define VT_ACTIVATE   0x5606UL
#define VT_WAITACTIVE 0x5607UL
#define VT_DISALLOCATE 0x5608UL

#define TUNIX_KB_101     0x02
#define TUNIX_KD_TEXT     0
#define TUNIX_KD_GRAPHICS 1
#define TUNIX_K_RAW       0
#define TUNIX_K_XLATE     1
#define TUNIX_K_MEDIUMRAW 2
#define TUNIX_K_UNICODE   3
#define TUNIX_K_OFF       4
#define TUNIX_VT_AUTO     0
#define TUNIX_VT_PROCESS  1

struct tunix_vt_stat {
    uint16_t v_active;
    uint16_t v_signal;
    uint16_t v_state;
};

struct tunix_vt_mode {
    uint8_t mode;
    uint8_t waitv;
    int16_t relsig;
    int16_t acqsig;
    int16_t frsig;
};

#define TTY_ISIG    0x00000001U
#define TTY_ICANON  0x00000002U
#define TTY_ECHO    0x00000008U
#define TTY_ECHOE   0x00000010U
#define TTY_ECHOK   0x00000020U
#define TTY_IEXTEN  0x00008000U

#define TTY_VINTR   0
#define TTY_VQUIT   1
#define TTY_VERASE  2
#define TTY_VKILL   3
#define TTY_VEOF    4
#define TTY_VTIME   5
#define TTY_VMIN    6
#define TTY_VSTART  8
#define TTY_VSTOP   9
#define TTY_VSUSP   10
#define TTY_NCCS    32

struct tunix_termios {
    uint32_t iflag;
    uint32_t oflag;
    uint32_t cflag;
    uint32_t lflag;
    uint8_t line;
    uint8_t cc[TTY_NCCS];
    uint32_t ispeed;
    uint32_t ospeed;
};

/*
 * One terminal's line discipline: what has been typed at it, what it has been
 * told about echoing and signals, and the screen it prints on. One per virtual
 * terminal, created by the VT layer.
 */
struct tty;
struct terminal_screen;

struct tty *tty_create(struct terminal_screen *screen);
void tty_destroy(struct tty *tty);
struct terminal_screen *tty_screen(const struct tty *tty);

int64_t tty_read(struct tty *tty, size_t size, void *buffer);
int64_t tty_write(struct tty *tty, size_t size, const void *buffer);
int tty_input_ready(struct tty *tty);
/*
 * Everything typed at the keyboard while this terminal is the active one, as a
 * keycode -- which is what the keymap is indexed by, and the only thing a USB
 * keyboard can produce. The PS/2 driver decodes its scancodes before this.
 */
void tty_handle_key(struct tty *tty, uint16_t keycode, int pressed);
/* One byte that arrived on the serial line. */
void tty_push_serial(struct tty *tty, uint8_t value);
/* The modifiers the keyboard is holding are global -- there is one keyboard --
   and this is how a change of ownership says the record is stale. */
void tty_reset_keyboard_state(void);
void tty_flush_input(struct tty *tty);

/* The termios and job-control ioctls. The KD and VT ones belong to the virtual
   terminal rather than the line discipline and live in vt.c. */
int tty_ioctl(struct tty *tty, unsigned long request, void *argument);

int tty_foreground_pgid(const struct tty *tty);
void tty_set_foreground_pgid(struct tty *tty, int pgid);
uint64_t tty_session(const struct tty *tty);
/* TIOCSCTTY: the session takes this terminal, and its leader takes the
   foreground. Job control is only enforced against this session. */
void tty_set_controlling_session(struct tty *tty, uint64_t sid, int pgid);
void tty_release_controlling_session(struct tty *tty, uint64_t sid);

#endif
