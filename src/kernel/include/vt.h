#ifndef TUNIX_VT_H
#define TUNIX_VT_H

#include <stddef.h>
#include <stdint.h>

/*
 * Virtual terminals.
 *
 * Every terminal has its own screen, its own line discipline and its own idea
 * of who is in the foreground; one of them is active, and only that one is on
 * the display and fed by the keyboard. Ctrl+Alt+F1..F8 moves between them, and
 * so does VT_ACTIVATE for a program that asks -- a display manager claims a
 * terminal of its own that way.
 *
 * Eight of them: six for the text logins a distribution usually has, and the
 * ones above that for a display manager and whatever it starts.
 */
#define VT_COUNT 8U

/* What a device node's `data` says about which terminal it means. A terminal
   number is itself; these two are the indirections Linux has. */
#define VT_NODE_ACTIVE 0U    /* /dev/tty0 and /dev/console: whichever is active */
#define VT_NODE_CURRENT 0xFFU /* /dev/tty: the caller's controlling terminal */

struct tty;
struct vfs_node;

/* Creates the first terminal and puts it on the display. The framebuffer
   terminal has to be initialised first. */
void vt_init(void);

/* The terminal by number, brought into existence if this is the first ask.
   NULL for a number out of range, or when there is no memory for it. */
struct tty *vt_tty(unsigned index);
struct tty *vt_active_tty(void);
unsigned vt_active_index(void);
/* The terminal the calling process belongs to: the one its session controls,
   or the active one when it has none. */
unsigned vt_current_index(void);
struct tty *vt_current_tty(void);

/* Make `index` the active terminal. Returns 0 when it is active, or when a
   VT_PROCESS owner has been asked to release the display and the switch will
   finish as soon as it answers. */
int vt_switch(unsigned index);
/* VT_WAITACTIVE: 0 once active, -EAGAIN while a switch is still in flight. */
int vt_wait_active(unsigned index);
/* What a caller waiting for a switch sleeps on. */
const void *vt_switch_wait_channel(void);
/*
 * What a blocked read on a terminal sleeps on, and what the keyboard wakes.
 * One channel for all of them rather than one each: a woken reader checks its
 * own terminal and goes back to sleep if the keystroke was not for it, and the
 * alternative is several login prompts spinning through the scheduler waiting
 * for somebody to type at one of them.
 */
const void *vt_input_wait_channel(void);
void vt_input_arrived(void);

/* A key, before anything else sees it. 1 when the VT layer consumed it, which
   is when it is one of the switching combinations. */
int vt_handle_hotkey(uint16_t keycode, int pressed, int ctrl_held, int alt_held);
/* A key the console should cook, for the active terminal. */
void vt_handle_key(uint16_t keycode, int pressed);
/*
 * Serial input and device polling, into whichever terminal is active. Cheap
 * enough for the timer interrupt, which is what notices the input that raises
 * no interrupt of its own -- the serial line and the USB event ring.
 */
void vt_poll_input(void);
void vt_poll_serial(void);
/* Whether an input reader bound to terminal `index` should be given events:
   the keyboard belongs to the active terminal, and to nothing else. */
int vt_input_delivered_to(unsigned index);

/* Display arbitration, called by framebuffer.c when the graphics owner
   changes. The terminal that was active at the time is the one the display
   belongs to, and switching away from it hands the screen back to the text
   console until it is switched back to. */
void vt_display_claimed(void);
void vt_display_released(void);
/* The terminal that holds the display because a program on it asked for
   KD_GRAPHICS, or NULL. See framebuffer_claim_graphics() for who asks. */
const void *vt_graphics_mode_owner(void);

/* A process that has gone: releases the VT_PROCESS ownership and the
   controlling-terminal claim it held, and unblocks a switch waiting on it. */
void vt_process_exited(uint64_t pid, uint64_t sid);

/* The device operations behind /dev/ttyN, /dev/tty0, /dev/tty and
   /dev/console. Which terminal is meant comes from the node. */
int64_t vt_node_read(struct vfs_node *node, uint64_t offset, size_t size, void *buffer);
int64_t vt_node_write(struct vfs_node *node, uint64_t offset, size_t size,
                      const void *buffer);
int vt_node_ready(struct vfs_node *node);
int64_t vt_node_ioctl(struct vfs_node *node, unsigned long request,
                      uint64_t user_argument);

#endif
