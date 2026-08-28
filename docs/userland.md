# The userland

Everything above the kernel is Void Linux, installed by Void's own package
manager into a directory on the build host and written into the image as an ext2
filesystem. Tunix builds no userland of its own.

It used to. There were about 130 ports, each a build script driving somebody
else's source against a musl cross-toolchain built here, and the whole thing
took hours and was one autotools change away from breaking. What it produced was
a distribution -- badly, and only for this project. Void already is one.

## What that changed

- **glibc, not musl.** The `void-x86_64-ROOTFS` tarball without `-musl` in its
  name is the glibc build, and every package installed on top of it follows.
- **runit, not dinit.** Void's init, unmodified: `/etc/runit/1` runs the core
  services, `/etc/runit/2` runs `runsvdir`, services live in `/etc/sv` and are
  enabled by a symlink into `/etc/runit/runsvdir/default`.
- **Nothing to patch.** The binaries in the image are the ones Void publishes.
  When one of them does not work, the kernel is wrong.

That last point is the reason for the change, and it paid immediately. glibc
gives the kernel a 36-byte `struct termios` on the stack and converts; musl
passes its own 60-byte one straight through. The kernel had been writing 60
bytes all along, and musl's layout hid it. Under glibc every program that called
`isatty()` -- which is every program that writes to a terminal -- died with
*stack smashing detected* before printing anything.

## base-files

`base-files/` is the layer that makes it Tunix:

| Path | What it is |
| --- | --- |
| `overlay/` | Copied over the tree: `/etc/hostname`, `/etc/os-release`, `/etc/fstab`, `/etc/rc.local`, weston's configuration and its service, the `tunix` user's shell configuration |
| `append/` | Appended to `/etc/passwd`, `/etc/group` and `/etc/shadow`, because Void's own packages own those files and add their system users to them |
| `services` | The runit services to enable. The directory is cleared first, so this list is the whole answer rather than an addition to what `runit-void` happened to enable |
| `remove` | Paths to delete: manuals, locales, documentation |

## What is enabled

- **udevd**, Void's `eudev`, started by the core services like anywhere else.
  It hears about devices over `NETLINK_KOBJECT_UEVENT` and writes what it
  decides to `/run/udev/data`, which is where libudev looks -- see
  [The desktop](desktop.md) for what that chain took.
- **seatd and weston**, which is the graphical session.
- **agetty** on the second, third and fourth virtual terminals. Not the first:
  weston takes whichever terminal is active when it starts.

## What is not enabled, and why

- **dhcpcd.** It needs a packet socket, which this network stack has no concept
  of. `/etc/rc.local` configures the interface statically instead, with the
  addresses QEMU's user-mode network hands out.

## The parts of the boot that still complain

A clean boot still prints a few failures, and they are all things Linux has and
this kernel does not:

- `mount: /sys/kernel/security` and `/sys/fs/cgroup` -- no LSM, no cgroups.
- `mount: /proc already mounted` and the five like it. The kernel mounts
  `/proc`, `/sys`, `/dev`, `/run`, `/dev/pts` and `/dev/shm` itself before init
  runs, so the init script's `mountpoint -q || mount` finds them already there
  and `mount` says so.

None of them stop the boot.

## Logging in

The machine comes up in weston. Ctrl+Alt+F2 moves to a text console, where both
accounts have the same password: `tunix` / `tunix` and `root` / `tunix`. The
hash is in `base-files/append/shadow` in plain sight, and is written over
root's as the sysroot is assembled -- this is a machine you boot in an emulator
to look at.

`tunix` is in `wheel`, so `sudo` works, and in `_seatd`, which is how the
session reaches the display.

## fastfetch

`fastfetch` is configured for the `tunix` account in
`~/.config/fastfetch/config.jsonc`, and draws the logo in
`/usr/share/fastfetch/logo.txt` beside the usual summary. Both files come from
`base-files/overlay`.

Nothing in the list is hardcoded: what a module cannot answer it leaves out, so
the same config prints a shorter block on a text console than it does in
weston-terminal, where the display, the compositor, the terminal and its font
all have an answer. There is no `Host` line at all -- it reads SMBIOS, and the
kernel exposes none.
