#ifndef TUNIX_TTY_H
#define TUNIX_TTY_H

#include <stddef.h>
#include <stdint.h>

#define TCGETS      0x5401UL
#define TCSETS      0x5402UL
#define TCSETSW     0x5403UL
#define TCSETSF     0x5404UL
#define TIOCGPGRP   0x540FUL
#define TIOCSPGRP   0x5410UL
#define TIOCGWINSZ  0x5413UL
#define TIOCGETD    0x5424UL
#define TIOCSETD    0x5423UL

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

void tty_init(void);
int tty_ioctl(unsigned long request, void *argument);
int tty_foreground_pgid(void);
void tty_set_foreground_pgid(int pgid);
int tty_input_ready(void);
void tty_poll_inputs(void);
void tty_handle_scancode(uint8_t scancode);
void tty_reset_keyboard_state(void);
int64_t tty_read(size_t size, void *buffer);
int64_t tty_write(size_t size, const void *buffer);

#endif
