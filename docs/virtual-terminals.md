# Virtual Terminals

Tunix has eight virtual terminals. Ctrl+Alt+F1 through Ctrl+Alt+F8 move between
them, `chvt` does the same from a program, and each one keeps its own screen,
its own keyboard queue and its own idea of who is in the foreground. The first
is the desktop; the second, third and fourth run a login prompt.

![A text console on Tunix](../screenshots/console.png)

The banner is `/etc/issue`, drawn out of half-block characters, and the colours
are agetty's `\e{...}` escapes landing on the palette in `kernel/tty/terminal.c`.

Before this there was one console. Every question about which terminal was
active had the same answer, `VT_ACTIVATE` refused anything but terminal 1, and
the display manager and the compositor had to be told to stop asking --
`XORG_NO_VT` and `SEATD_VTBOUND=0` exist because of that.

## What a terminal is

Three things, kept apart deliberately:

- **the screen** (`kernel/tty/terminal.c`), a grid of cells with a cursor and a
  scroll region. A screen draws to the display only while it is the active one,
  so a terminal nobody is looking at goes on scrolling into its own cells and
  shows the result the moment it is switched to.
- **the line discipline** (`kernel/tty/tty.c`), which is the termios flags, the
  input queue, the canonical line buffer, the foreground process group and the
  ANSI parser for what is written to it.
- **the terminal itself** (`kernel/tty/vt.c`), which owns those two and the
  state that only means anything when there is more than one of them: the KD
  mode, the VT mode, and which terminal the display belongs to.

Screens are allocated when a terminal is first used, and the cells are sized to
the display rather than to a maximum, so seven unused terminals cost nothing.
The alternate screen buffer is allocated only if a program asks for one.

## The device nodes

| node | means |
| --- | --- |
| `/dev/tty1` .. `/dev/tty8` | that terminal |
| `/dev/tty0` | whichever terminal is active |
| `/dev/console` | the same, and where the service manager writes |
| `/dev/tty` | the caller's own controlling terminal |

They are all the same four operations with the terminal carried in the node
(`vt_node_read` and friends), and they carry Linux's device numbers: major 4
for the terminals and tty0, major 5 for the two indirect nodes.

`/dev/tty` resolves through the caller's session: a process whose session has
claimed a terminal with `TIOCSCTTY` gets that one, and anything else gets the
active one.

One consequence worth stating: a character device that is *not* a terminal now
answers `ENOTTY` to the termios ioctls rather than answering for the console.
`isatty()` used to be true for `/dev/null`.

## Switching

`vt_switch` is the whole of it, and it is the same path whether a key or an
ioctl asked:

1. If the terminal being left has a `VT_PROCESS` owner, that process is sent
   its release signal and the switch stops there. It resumes when the owner
   answers with `VT_RELDISP`, or when it exits without answering.
2. The display is handed over (below).
3. The new terminal's screen is put on the display and painted.
4. A `VT_PROCESS` owner on the new terminal is sent its acquire signal.

`VT_WAITACTIVE` sleeps on a wait channel rather than spinning, and the switch
wakes it.

### Ctrl+Alt+F*n*

The combination is caught in `input.c`, in `keyboard_emit_key`, which is above
both the evdev readers and the console's own cooking. That placement is the
point: a compositor holding an exclusive grab must not be able to keep the user
from leaving it, and no shell should ever see the F-key that moved the screen.
Both the press and the release are swallowed, because half of a key is worse
than none of it.

It works for a USB keyboard as well as a PS/2 one -- the two meet at that
function, one decoded from scancodes and the other from HID usages -- and so,
below it, does everything else the console does with a key.

## The display

Only one thing draws on the screen at a time. An X server or a compositor takes
the display by presenting a frame (`framebuffer_claim_graphics`), and the
terminal that was active at that moment is the terminal the display belongs to.
Nothing announces it: with `XORG_NO_VT` the X server never binds a terminal at
all, so what it is *on* is where the user was when it started drawing.

Switching away from that terminal **suspends** the owner rather than taking the
claim away. It is still running and still owns its buffers; it simply stops
reaching the screen, and the text console is allowed to draw again. Switching
back resumes it and puts the last frame it presented back up -- the client has
been drawing all along and has no reason to think anything changed.

With a virtio-gpu the suspend also hands the scanout back, because on that
device the client's buffer is the scanout and the console's framebuffer is only
on screen while no resource is bound to it.

A program that has `/dev/fb0` mapped is the exception: the mapping is the real
scanout, so it goes on writing to the screen across a switch. Nothing on the
image does that outside `fb-test`.

## Input

Keystrokes go to the active terminal, and nowhere else.

The console is fed **keycodes**, not scancodes. That is what the keymap has
always been indexed by -- `loadkeys` reads "keycode N = symbol" out of a keymap
file, as on Linux -- and feeding it scancodes only worked because the two
coincide for the main block of a set-1 keyboard. The PS/2 driver decodes its
scancodes into keycodes anyway, and a USB keyboard produces nothing else, so
both meet at `keyboard_emit_key()` and both can type at a login prompt. Before
this, a machine whose only keyboard was USB could switch terminals but not type
at one: the console was on the scancode path, which only the PS/2 driver fed.

A machine with no i8042 at all is a real machine, not just `i8042=off` in QEMU.
An absent controller answers every port with 0xFF, which reads as "there is a
byte waiting" for ever, so the controller is probed once at init and the drain
loop stops on it. The pointer device node is created when there is a pointer of
*either* kind, which is why a USB mouse gets `/dev/input/event1`.

For the console that is obvious. For evdev it is not: an X server keeps
`/dev/input/event0` open the whole time it is running, including while the user
is typing a password at a login on another terminal. Every reader is therefore
bound to the terminal its opener was on, and gets events only while that
terminal is active. This is the job logind does on a Linux desktop, and there
is nothing else here to do it.

The console stops cooking keystrokes when an input reader bound to the *active*
terminal exists -- which is how a terminal running a graphical session does not
also feed the shell underneath it.

## Blocking, and what wakes it

A read on a terminal with nothing typed at it sleeps on a wait channel that the
keyboard wakes (`vt_input_wait_channel`). It used to rewind and retry on every
schedule, which was tolerable for one console and would have been four
processors spinning on nothing with a login prompt on each of four terminals.

That left the input nobody polls: the serial line raises no interrupt here, and
USB is an event ring that is read rather than delivered. The timer interrupt
polls both, 250 times a second, at the cost of one port read and one memory
read when there is nothing there.

## The handshake, with a compositor in it

Weston is on the image now, and it is the first program here that registers as
a `VT_PROCESS` owner -- through seatd, which is what actually calls
`VT_SETMODE` and answers `VT_RELDISP`. Pressing Ctrl+Alt+F2 on a running
desktop goes:

1. The keyboard interrupt reaches `vt_handle_hotkey()`, which takes the key
   before evdev sees it -- a compositor with the keyboard grabbed must not be
   able to keep the user from leaving it.
2. `vt_switch()` finds terminal 1 in `VT_PROCESS` mode and sends seatd the
   release signal it asked for, then waits.
3. seatd tells weston to disable the session; weston drops DRM master and says
   so; seatd answers `VT_RELDISP`.
4. The switch finishes, and the console is put back on the screen.

Two bugs sat between the design and it working. The kernel refused to send a
signal at all when the processor taking the interrupt had no current process --
which is what an idle processor is, and what a machine showing a still desktop
usually has. And the console did not come back afterwards, because handing the
scanout back does not return a virtio-gpu to the framebuffer underneath; see
[virtio-gpu](virtio-gpu.md).

## On the image

`agetty` from util-linux is what makes a terminal a *login* terminal: it starts
a session, opens the terminal, claims it with `TIOCSCTTY`, puts it on the
standard descriptors and executes `login`. Three runit services, `agetty-tty2`
to `agetty-tty4`, run one each and restart it when the session ends; which ones
is `base-files/services`.

Terminal 1 has none. Weston takes whichever terminal is active when it starts,
which is the first, and a login prompt sharing it would draw into the same
cells -- see [The desktop](desktop.md).

`chvt`, from the kbd package, switches to a numbered terminal.

## What is not here

- **A terminal cannot be resized.** All eight are the size of the display.
- **Scrollback.** Shift+PageUp does nothing; a terminal holds exactly what fits
  on the screen.
- **`KDSETMODE(KD_GRAPHICS)` from a terminal that is not active** is refused
  rather than deferred. Nothing on the image asks.
- **Nothing waits for a terminal that refuses to leave.** `VT_RELDISP` with 0
  cancels the pending switch and the request is dropped rather than retried;
  Linux would keep the terminal and report it. Nothing on the image refuses.
- **Echo is per line, not per key.** The line discipline echoes when the line is
  read, not as it is typed, which predates this work and is unchanged by it.

## The serial line, and machines that do not have one

The tick polls the serial port, because a byte arriving on it raises no
interrupt anything here unmasks. That poll used to be:

```c
if (inb(0x3FD) & 1U) return inb(0x3F8);   /* line status bit 0: a byte waits */
```

in a loop until it said no. On a machine with nothing decoding 0x3F8 every read
answers 0xFF, and 0xFF says a byte is waiting as loudly as a real byte does --
so the loop never ended. It ran inside the timer interrupt, holding the kernel
lock, which is a machine that stops a fraction of a second after the first
tick, having printed whatever it had already printed, with no fault and no
message. Every other processor then piled up behind the lock:

```
KLOCK: cpu 1 stuck waiting for ticket 28: next 31 serving 27 shared 0
KLOCK: cpu 0 holds 1 doing 20020
```

`20020` is interrupt vector 0x20, the tick. That is the whole diagnosis.

An emulator never shows it, because an emulator always has the port. The PS/2
driver has guarded against the identical trap since it was written -- "all ones
is an absent controller, not a full output buffer" -- and this one had not.

So `serial_init()` probes the scratch register, which is the one register of a
16550 that does nothing but remember what was written to it, and everything
else is a no-op when there is no port. The waits are bounded as well: a port
that is present but never reports its transmit register empty, and a port that
answers every read with a byte, both cost a tick rather than the machine.
