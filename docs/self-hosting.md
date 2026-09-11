# Self-Hosting

Tunix compiles Tunix. Clone the repository inside the running system, run
`make kernel`, and the `build/kernel.elf` that comes out boots the desktop.
It is not byte for byte the host's build of the same commit, because the image
carries Void's gcc 14 and the host that built the image may carry any other --
nothing in the tree stamps a date or a revision into the binary.

```sh
git clone --depth 1 https://github.com/tuna4ll/tunix.git
cd tunix
make -j4 kernel
```

## What the image carries

`VOID_INSTALL_TOOLCHAIN` in the GNUmakefile adds the packages the kernel build
asks for, and nothing else:

| Package | Why |
| --- | --- |
| `gcc`, `binutils` | the compiler and the linker |
| `make` | the build itself |
| `python3`, `python3-Pillow` | `support/terminal-font.py` rasterises the console font |
| `git` | the clone, and the Limine checkout the build makes for itself |

The kernel takes its boot protocol header from Limine, and the header and the
bootloader that reads it have to be the same version, so `make kernel` clones
`build/limine` before it compiles anything. That and the clone of Tunix are the
two things the build needs a network for.

`make image` is a different matter and does not run here. It installs a Void
sysroot with xbps and partitions a disk, and Tunix has neither xbps nor the
loop devices `support/image.sh` wants.

## What had to work first

Four things stood between a toolchain in the image and a kernel out of it.
Every one of them was a place where Tunix answered a system call in a way Linux
does not, and every one of them had been sitting there unnoticed because
nothing else in the userland asked.

### getcwd returns a length

`getcwd(NULL, 0)` is glibc's allocating form: it asks the kernel to fill a
buffer and takes the answer as the size to shrink its allocation to. Tunix
returned the buffer pointer instead, which glibc reads as a failure with no
errno set. Every build opened with `make: getcwd: Success` and then built from
whatever directory make had guessed.

### A parallel make waits in pselect

GNU make 4.4 hands out job slots through a jobserver. A job that wants a slot
blocks `SIGCHLD`, then waits in `pselect6` with a signal mask that unblocks it
again for exactly as long as the wait lasts, so a child exiting interrupts the
wait and nothing is missed in between.

Tunix ignored the mask argument entirely, so `SIGCHLD` stayed blocked, and a
child's exit only ever woke a parent that was sitting in `wait4`. `make -j4`
slept forever with three compilers behind it as zombies:

```
529 S  08:35 make
553 Z  08:30 gcc <defunct>
554 Z  08:30 gcc <defunct>
555 Z  08:30 gcc <defunct>
```

`pselect6` and `ppoll` now swap in the mask they are given for the length of
the wait and put the old one back before they return, and a child's exit wakes
a parent blocked anywhere -- not only in `wait4` -- whenever `SIGCHLD` would
actually reach a handler. `Ctrl+Alt+D` printed the state that showed which of
the two it was: `make` was in `io-syscall=270`, not in `wait4`.

### The link line is ninety arguments

`execve` staged its arguments in a fixed 64 by 4096 array, which is one
argument for every eight the final link passes:

```
cc -nostdlib ... build/kernel/abi_gaps.c.o ... build/kernel/vmm.c.o -o build/kernel.elf
```

Eighty-four objects and the flags around them. The failure was quiet -- make
reported `Error 127` and printed nothing else -- because make execs a recipe
with no shell characters in it directly and does not say why the exec failed.
Run by hand under `/bin/sh` it says what it is:

```
/root/link.sh: 1: cc: Argument list too long
```

Arguments and environment now share one 256 KiB pool, up to 512 entries each.
The old array cost half a megabyte per `execve` to hold 64; the pool costs half
that to hold eight times as many, because it stores the strings end to end
rather than giving each one a 4 KiB slot it will not use.

### connect answers once

A blocking `connect` on a stream socket sleeps and re-runs itself until the
handshake finishes. The handshake also marks the socket connected, and the
retry took that as proof it had already answered somebody -- so on a connection
that came up fast enough, the call that should have returned success returned
`EISCONN`. Loopback and the QEMU user network are both that fast:

```
-bash: connect: Transport endpoint is already connected
```

Whether the socket is connected and whether `connect` has said so are now two
different facts.

## Doing it twice

A kernel built inside Tunix boots into the desktop, Firefox and all. Put that
kernel in the image, boot it, and build again, and the second kernel is byte
for byte the first:

![The second generation](../screenshots/self-host.png)

That is the whole claim. The first generation only shows the toolchain runs;
the second shows it runs the same way the host's did, because a kernel that
compiled itself wrong would not hash the same as the one that compiled it.

Building inside a kernel from before these four fixes does not get that far. It
stops where `make -j4` stopped in the first place, with three compilers exited
and nobody left awake to reap them.
