#include <stddef.h>
#include <stdint.h>
#include "include/drm.h"
#include "include/framebuffer.h"
#include "include/input.h"
#include "include/io.h"
#include "include/kstring.h"
#include "include/process.h"
#include "include/signal.h"
#include "include/terminal.h"
#include "include/tty.h"
#include "include/usercopy.h"
#include "include/vfs.h"
#include "include/vt.h"
#include "../include/tunix/input_event.h"

#define EPERM 1
#define ENXIO 6
#define EAGAIN 11
#define EFAULT 14
#define EBUSY 16
#define EINVAL 22
#define ENOTTY 25
#define ENOMEM 12

struct linux_winsize {
    uint16_t rows;
    uint16_t columns;
    uint16_t pixel_width;
    uint16_t pixel_height;
};

/*
 * One virtual terminal. The line discipline and the screen are the terminal
 * itself; the rest is what a program that drives the display has told us about
 * how it wants to be treated when the terminal is switched away from.
 */
struct vt {
    struct tty *tty;
    struct terminal_screen *screen;
    struct tunix_vt_mode mode;
    /* The process that asked for VT_PROCESS. Signals go to it, and its exit is
       what releases a switch that is waiting for an answer. */
    uint64_t owner_pid;
    int kd_mode;
    int kb_mode;
    int allocated;
};

static struct vt terminals[VT_COUNT + 1U];
static unsigned active_index = 1U;
/*
 * A switch that has been asked for but not finished: the terminal being left
 * runs a program that wanted to be told first (VT_SETMODE with VT_PROCESS), so
 * it has been signalled and the switch happens when it answers with VT_RELDISP.
 */
static unsigned pending_index;
/*
 * Which terminal the display belongs to while something other than the text
 * console is drawing on it. Set from the terminal that was active when the
 * claim was made -- an X server does not announce which terminal it is on, it
 * simply starts presenting, and the one in front of the user at that moment is
 * the one it is presenting to.
 */
static unsigned display_owner_index;
static int display_suspended;
static char switch_channel;
static char input_channel;

extern void kprintf(const char *fmt, ...);

static int index_valid(unsigned index) {
    return index >= 1U && index <= VT_COUNT;
}

static struct vt *vt_ensure(unsigned index) {
    if (!index_valid(index)) return NULL;
    struct vt *vt = &terminals[index];
    if (vt->allocated) return vt;
    vt->screen = terminal_screen_create();
    if (!vt->screen) return NULL;
    vt->tty = tty_create(vt->screen);
    if (!vt->tty) {
        terminal_screen_destroy(vt->screen);
        vt->screen = NULL;
        return NULL;
    }
    vt->kd_mode = TUNIX_KD_TEXT;
    vt->kb_mode = TUNIX_K_XLATE;
    vt->mode.mode = TUNIX_VT_AUTO;
    vt->allocated = 1;
    return vt;
}

struct tty *vt_tty(unsigned index) {
    struct vt *vt = vt_ensure(index);
    return vt ? vt->tty : NULL;
}

unsigned vt_active_index(void) { return active_index; }

struct tty *vt_active_tty(void) { return terminals[active_index].tty; }

unsigned vt_current_index(void) {
    struct process *process = process_current();
    if (process && process->sid) {
        for (unsigned index = 1U; index <= VT_COUNT; index++) {
            if (terminals[index].allocated &&
                tty_session(terminals[index].tty) == process->sid)
                return index;
        }
    }
    return active_index;
}

struct tty *vt_current_tty(void) { return vt_tty(vt_current_index()); }

const void *vt_switch_wait_channel(void) { return &switch_channel; }

const void *vt_input_wait_channel(void) { return &input_channel; }

void vt_input_arrived(void) { (void)process_wake_all(&input_channel); }

/*
 * Hand the display over.
 *
 * A graphics client keeps its claim across a switch -- it is still running, it
 * still owns its buffers -- but while another terminal is in front it must not
 * reach the screen, and the console must be allowed to. Suspending is what
 * separates the two: the claim stands, the drawing stops.
 */
static void display_to_console(void) {
    if (!display_owner_index || display_suspended) return;
    drm_display_suspend();
    framebuffer_suspend_graphics();
    display_suspended = 1;
}

static void display_to_owner(void) {
    if (!display_owner_index || !display_suspended) return;
    framebuffer_resume_graphics();
    display_suspended = 0;
    /* The client has been drawing into its own buffer all along and has no
       reason to think anything changed, so the last frame it presented is put
       back on the screen for it. */
    drm_display_resume();
}

static void finish_switch(unsigned target) {
    struct vt *to = vt_ensure(target);
    if (!to) return;

    /* Both terminals' idea of which modifiers are held is stale: the Ctrl and
       Alt that did the switching are released while the other one is in front,
       and a terminal that thinks Ctrl is still down turns the next keystroke
       into a control character. What was *typed* at the terminal being left
       stays in it -- coming back to a half-written command line is the point of
       having several. */
    tty_reset_keyboard_state();

    active_index = target;
    pending_index = 0;

    if (display_owner_index == target) {
        display_to_owner();
        terminal_screen_activate(to->screen);
    } else {
        display_to_console();
        terminal_screen_activate(to->screen);
    }

    if (to->mode.mode == TUNIX_VT_PROCESS && to->mode.acqsig && to->owner_pid)
        (void)process_send_signal((int64_t)to->owner_pid, to->mode.acqsig);

    (void)process_wake_all(&switch_channel);
}

int vt_switch(unsigned index) {
    if (!index_valid(index)) return -EINVAL;
    if (!vt_ensure(index)) return -ENOMEM;
    if (index == active_index && !pending_index) return 0;

    struct vt *from = &terminals[active_index];
    /* A program that asked to be told has to be told, and has to answer, before
       the display moves. Xorg drops its DRM master and stops drawing in that
       window; a compositor does the same. */
    if (from->mode.mode == TUNIX_VT_PROCESS && from->mode.relsig &&
        from->owner_pid && process_find(from->owner_pid)) {
        pending_index = index;
        (void)process_send_signal((int64_t)from->owner_pid, from->mode.relsig);
        return 0;
    }
    finish_switch(index);
    return 0;
}

/* VT_RELDISP: 1 (or 2, which is "and I am done with it") allows the pending
   switch, 0 refuses it. Anything else is a program answering a question that
   was never asked. */
static int vt_release_display(int allow) {
    if (!pending_index) return allow ? 0 : -EINVAL;
    if (!allow) {
        pending_index = 0;
        (void)process_wake_all(&switch_channel);
        return 0;
    }
    finish_switch(pending_index);
    return 0;
}

int vt_wait_active(unsigned index) {
    if (!index_valid(index)) return -EINVAL;
    if (index == active_index) return 0;
    /* Nothing is on its way there, so waiting would be waiting forever. */
    if (pending_index != index) return -EINVAL;
    return -EAGAIN;
}

void vt_process_exited(uint64_t pid, uint64_t sid) {
    for (unsigned index = 1U; index <= VT_COUNT; index++) {
        struct vt *vt = &terminals[index];
        if (!vt->allocated) continue;
        if (sid) tty_release_controlling_session(vt->tty, sid);
        if (vt->owner_pid != pid) continue;
        vt->owner_pid = 0;
        vt->mode.mode = TUNIX_VT_AUTO;
        /* It died holding up a switch. Nobody is left to answer for it, and
           leaving the machine on a terminal whose owner is gone is worse than
           completing the move it was asked about. */
        if (pending_index && index == active_index) finish_switch(pending_index);
    }
}

void vt_display_claimed(void) {
    display_owner_index = active_index;
    display_suspended = 0;
}

void vt_display_released(void) {
    display_owner_index = 0;
    display_suspended = 0;
}

const void *vt_graphics_mode_owner(void) {
    if (!index_valid(display_owner_index)) return NULL;
    struct vt *vt = &terminals[display_owner_index];
    if (!vt->allocated || vt->kd_mode != TUNIX_KD_GRAPHICS) return NULL;
    return vt;
}

/*
 * Ctrl+Alt+F1..F8, ahead of everything else.
 *
 * This is deliberately above the input readers rather than below them: a
 * compositor with the keyboard grabbed must not be able to keep the user from
 * leaving it, which is exactly the situation where switching terminals matters
 * most. It is also above the console's own cooking, so the combination never
 * arrives at a shell as three separate keys.
 */
int vt_handle_hotkey(uint16_t keycode, int pressed, int ctrl_held, int alt_held) {
    if (!ctrl_held || !alt_held) return 0;
    unsigned target;
    if (keycode >= TUNIX_KEY_F1 && keycode <= TUNIX_KEY_F10)
        target = (unsigned)(keycode - TUNIX_KEY_F1) + 1U;
    else if (keycode == TUNIX_KEY_F11) target = 11U;
    else if (keycode == TUNIX_KEY_F12) target = 12U;
    else return 0;
    if (!index_valid(target)) return 0;
    if (pressed) (void)vt_switch(target);
    /* The release is swallowed too: a program that saw only half of a key it
       never saw pressed is a program that thinks the key is stuck. */
    return 1;
}

void vt_handle_key(uint16_t keycode, int pressed) {
    tty_handle_key(terminals[active_index].tty, keycode, pressed);
}

int vt_input_delivered_to(unsigned index) {
    return index == 0U || index == active_index;
}

/* One byte off the serial line, or nothing. The serial port is a keyboard like
   any other as far as the terminals are concerned, and it types at whichever
   one is active. */
static int serial_try_read(void) {
    if (inb(0x3FD) & 1U) return inb(0x3F8);
    return -1;
}

void vt_poll_serial(void) {
    struct tty *tty = terminals[active_index].tty;
    if (!tty) return;
    for (;;) {
        int value = serial_try_read();
        if (value < 0) break;
        tty_push_serial(tty, (uint8_t)value);
    }
}

void vt_poll_input(void) {
    vt_poll_serial();
    input_poll();
}

void vt_init(void) {
    memset(terminals, 0, sizeof(terminals));
    active_index = 1U;
    pending_index = 0;
    display_owner_index = 0;
    display_suspended = 0;
    struct vt *first = vt_ensure(1U);
    if (!first) return;
    terminal_screen_activate(first->screen);
}

/* Which terminal a device node names. */
static struct vt *vt_from_node(struct vfs_node *node) {
    unsigned index = node ? (unsigned)(uintptr_t)node->data : VT_NODE_ACTIVE;
    if (index == VT_NODE_ACTIVE) index = active_index;
    else if (index == VT_NODE_CURRENT) index = vt_current_index();
    return vt_ensure(index);
}

int64_t vt_node_read(struct vfs_node *node, uint64_t offset, size_t size, void *buffer) {
    (void)offset;
    struct vt *vt = vt_from_node(node);
    if (!vt) return -ENXIO;
    if (!tty_input_ready(vt->tty)) return -EAGAIN;
    return tty_read(vt->tty, size, buffer);
}

int64_t vt_node_write(struct vfs_node *node, uint64_t offset, size_t size,
                      const void *buffer) {
    (void)offset;
    struct vt *vt = vt_from_node(node);
    if (!vt) return -ENXIO;
    return tty_write(vt->tty, size, buffer);
}

int vt_node_ready(struct vfs_node *node) {
    struct vt *vt = vt_from_node(node);
    return vt ? tty_input_ready(vt->tty) : 0;
}

/* KD_GRAPHICS is a program announcing it will paint the screen itself, so the
   console stands down -- the same handover DRM performs when a client presents
   its first frame. KD_TEXT gives the display back and the console redraws. */
static int64_t set_kd_mode(struct vt *vt, int mode) {
    if (mode != TUNIX_KD_TEXT && mode != TUNIX_KD_GRAPHICS) return -EINVAL;
    if (mode == vt->kd_mode) return 0;
    if (mode == TUNIX_KD_GRAPHICS) {
        /* Only the terminal in front may take the display: a claim from one
           the user cannot see would black out the one they are looking at. */
        if (vt != &terminals[active_index]) return -EBUSY;
        if (framebuffer_claim_graphics(vt) != 0) return -EBUSY;
    } else {
        framebuffer_release_graphics(vt, 0);
    }
    vt->kd_mode = mode;
    return 0;
}

static unsigned vt_first_free(void) {
    for (unsigned index = 1U; index <= VT_COUNT; index++)
        if (!terminals[index].allocated) return index;
    return 0;
}

static uint16_t vt_in_use_mask(void) {
    uint16_t mask = 0;
    for (unsigned index = 1U; index <= VT_COUNT; index++)
        if (terminals[index].allocated) mask |= (uint16_t)(1U << index);
    return mask;
}

int64_t vt_node_ioctl(struct vfs_node *node, unsigned long request,
                      uint64_t user_argument) {
    struct vt *vt = vt_from_node(node);
    if (!vt) return -ENXIO;

    switch (request) {
        case TCGETS: {
            struct tunix_termios value;
            if (tty_ioctl(vt->tty, request, &value) != 0) return -ENOTTY;
            return copy_to_user(user_argument, &value, sizeof(value)) == 0 ? 0 : -EFAULT;
        }
        case TCSETS:
        case TCSETSW:
        case TCSETSF: {
            struct tunix_termios value;
            if (copy_from_user(&value, user_argument, sizeof(value)) != 0) return -EFAULT;
            return tty_ioctl(vt->tty, request, &value) == 0 ? 0 : -ENOTTY;
        }
        case TIOCGPGRP: {
            int pgid;
            if (tty_ioctl(vt->tty, request, &pgid) != 0) return -ENOTTY;
            return copy_to_user(user_argument, &pgid, sizeof(pgid)) == 0 ? 0 : -EFAULT;
        }
        case TIOCSPGRP: {
            int pgid;
            if (copy_from_user(&pgid, user_argument, sizeof(pgid)) != 0) return -EFAULT;
            return tty_ioctl(vt->tty, request, &pgid) == 0 ? 0 : -ENOTTY;
        }
        case TIOCGETD:
        case TIOCSETD: {
            int discipline = 0;
            if (request == TIOCSETD &&
                copy_from_user(&discipline, user_argument, sizeof(discipline)) != 0)
                return -EFAULT;
            if (tty_ioctl(vt->tty, request, &discipline) != 0) return -ENOTTY;
            if (request == TIOCGETD &&
                copy_to_user(user_argument, &discipline, sizeof(discipline)) != 0)
                return -EFAULT;
            return 0;
        }
        /* login(1) claims a terminal as its session's controlling terminal
           before handing it to the user's shell; su(1) gives it up again. */
        case TIOCSCTTY: {
            struct process *process = process_current();
            if (!process) return -ENOTTY;
            tty_set_controlling_session(vt->tty, process->sid, (int)process->pgid);
            return 0;
        }
        case TIOCNOTTY: {
            struct process *process = process_current();
            if (!process) return -ENOTTY;
            tty_release_controlling_session(vt->tty, process->sid);
            return 0;
        }
        case TIOCGWINSZ: {
            struct linux_winsize winsize;
            memset(&winsize, 0, sizeof(winsize));
            terminal_get_dimensions(&winsize.rows, &winsize.columns);
            return copy_to_user(user_argument, &winsize, sizeof(winsize)) == 0 ? 0 : -EFAULT;
        }
        case TUNIX_KDGKBMAP: {
            struct tunix_keymap map;
            if (tty_ioctl(vt->tty, request, &map) != 0) return -ENOTTY;
            return copy_to_user(user_argument, &map, sizeof(map)) == 0 ? 0 : -EFAULT;
        }
        case TUNIX_KDSKBMAP: {
            struct tunix_keymap map;
            if (copy_from_user(&map, user_argument, sizeof(map)) != 0) return -EFAULT;
            return tty_ioctl(vt->tty, request, &map) == 0 ? 0 : -EINVAL;
        }
        case KDGKBTYPE: {
            uint8_t type;
            if (tty_ioctl(vt->tty, request, &type) != 0) return -ENOTTY;
            return copy_to_user(user_argument, &type, sizeof(type)) == 0 ? 0 : -EFAULT;
        }
        /* The argument of these is the value itself, not a pointer to it. */
        case KDSETMODE:
            return set_kd_mode(vt, (int)user_argument);
        case KDSKBMODE: {
            int mode = (int)user_argument;
            if (mode < TUNIX_K_RAW || mode > TUNIX_K_OFF) return -EINVAL;
            vt->kb_mode = mode;
            return 0;
        }
        case KDGETMODE: {
            int mode = vt->kd_mode;
            return copy_to_user(user_argument, &mode, sizeof(mode)) == 0 ? 0 : -EFAULT;
        }
        case KDGKBMODE: {
            int mode = vt->kb_mode;
            return copy_to_user(user_argument, &mode, sizeof(mode)) == 0 ? 0 : -EFAULT;
        }
        case VT_OPENQRY: {
            unsigned free_index = vt_first_free();
            if (!free_index) return -ENXIO;
            int value = (int)free_index;
            return copy_to_user(user_argument, &value, sizeof(value)) == 0 ? 0 : -EFAULT;
        }
        case VT_GETSTATE: {
            struct tunix_vt_stat state;
            state.v_active = (uint16_t)active_index;
            state.v_signal = 0;
            state.v_state = vt_in_use_mask();
            return copy_to_user(user_argument, &state, sizeof(state)) == 0 ? 0 : -EFAULT;
        }
        case VT_GETMODE:
            return copy_to_user(user_argument, &vt->mode, sizeof(vt->mode)) == 0 ? 0 : -EFAULT;
        case VT_SETMODE: {
            struct tunix_vt_mode mode;
            if (copy_from_user(&mode, user_argument, sizeof(mode)) != 0) return -EFAULT;
            if (mode.mode != TUNIX_VT_AUTO && mode.mode != TUNIX_VT_PROCESS) return -EINVAL;
            if (mode.mode == TUNIX_VT_PROCESS &&
                (mode.relsig < 0 || mode.relsig > 64 ||
                 mode.acqsig < 0 || mode.acqsig > 64)) return -EINVAL;
            vt->mode = mode;
            struct process *process = process_current();
            vt->owner_pid = (mode.mode == TUNIX_VT_PROCESS && process) ? process->pid : 0;
            return 0;
        }
        case VT_ACTIVATE:
            return vt_switch((unsigned)user_argument);
        case VT_WAITACTIVE:
            return vt_wait_active((unsigned)user_argument);
        case VT_RELDISP:
            return vt_release_display((int)user_argument);
        /* VT_DISALLOCATE frees a terminal nobody is using. Refusing to free the
           active one is what Linux does; freeing the rest is a courtesy, and a
           terminal comes back the moment anything opens it again. */
        case VT_DISALLOCATE: {
            unsigned index = (unsigned)user_argument;
            if (!index) return 0;
            if (!index_valid(index)) return -EINVAL;
            if (index == active_index) return -EBUSY;
            struct vt *target = &terminals[index];
            if (!target->allocated) return 0;
            if (display_owner_index == index) return -EBUSY;
            tty_destroy(target->tty);
            terminal_screen_destroy(target->screen);
            memset(target, 0, sizeof(*target));
            return 0;
        }
        default:
            return -ENOTTY;
    }
}
