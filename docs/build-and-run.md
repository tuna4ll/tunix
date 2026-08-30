# Build and Run

```sh
make          # kernel + sysroot + image
make run      # boot it under BIOS
```

The first build downloads a Void Linux rootfs and about 300 MiB of packages, so
it needs a network. After that everything but the kernel is cached.

## What gets built

Only the kernel. Everything above it is a Void Linux package:

| Step | Target | What happens |
| --- | --- | --- |
| Kernel | `make kernel` | `kernel/` compiled to `build/kernel.elf` |
| Sysroot | `make sysroot` | `support/sysroot.sh` unpacks Void's ROOTFS tarball, installs `VOID_INSTALL` into it with xbps, and lays `base-files/` over the result |
| Image | `make image` | `support/image.sh` makes a GPT disk with an ESP and an ext2 root, and installs Limine into both |

Limine itself is cloned into `build/limine` on the first build: the header the
kernel builds its requests from and the bootloader that reads them have to be
the same version, so both come from one checkout.

## Requirements

- `gcc`, `binutils`, `make`, `python3`
- `curl`, `git`, `tar`
- `mtools`, `e2fsprogs`, `util-linux` (`sfdisk`), `coreutils`
- `qemu-system-x86_64` to run it
- root, for `make sysroot` — the tarball carries ownership and the image has to
  keep it

## The sysroot needs a real filesystem

`sysroot.sh` refuses to build on a filesystem that cannot record ownership or
the setuid bit, because the result would be an image where `sudo` is not setuid
and every file belongs to root. A Windows drive mounted into WSL is the usual
way to hit this, and the failure is otherwise silent -- `chmod` succeeds and
changes nothing. Point the sysroot somewhere else:

```sh
make image SYSROOT=/var/tmp/tunix/sysroot CACHE=/var/tmp/tunix/cache
```

Rather than typing that every time, put it in `local.mk`, which the GNUmakefile
includes if it exists and git ignores:

```make
SYSROOT = /var/tmp/tunix/sysroot
CACHE   = /var/tmp/tunix/cache
```

Then `make run` is `make run` again. The override is only needed when the
sysroot is actually rebuilt -- which any change to `GNUmakefile`,
`support/sysroot.sh` or `base-files/` causes -- but a stale setting is worse
than a redundant one, so the file is the better answer.

The packages themselves are cached in `$(CACHE)/packages` and survive a
rebuild, so adding one package does not fetch the other six hundred megabytes
again.

## Changing what is installed

`VOID_INSTALL` in the GNUmakefile is the package list, `VOID_REMOVE` is what to
take back out again, and `VOID_ROOTFS_DATE` is which base tarball to start from.

```sh
make image VOID_INSTALL="base-files bash coreutils util-linux runit runit-void git"
```

`base-files/` is what makes it Tunix rather than Void: `overlay/` is copied over
the tree, `append/` is appended to `/etc/passwd`, `/etc/group` and
`/etc/shadow`, `services` is the list of runit services to enable, and `remove`
is a list of paths to delete.

## The partition table

The image is GPT by default, which is what a UEFI machine expects and what
QEMU has always been given here. `IMAGE_TABLE=mbr` builds the same image with
an MBR instead, and marks the EFI system partition active:

```sh
rm -f build/tunix.img
make image IMAGE_TABLE=mbr
```

It is for one case, and it is a real one. An old BIOS booting from a USB stick
decides between hard-disk and floppy emulation by looking for an active
partition in the boot record; a GPT disk's protective MBR has none, so the
stick is presented as a floppy, and in that mode Limine's second stage is
handed a device with no partition table to search. What it says then is

```
!! Stage 3 file not found
PANIC: Failed to load stage 3.
```

which reads like a missing file and is really a missing partition table. The
MBR image boots under both firmwares -- OVMF takes an MBR ESP -- so nothing is
given up by using it, and the second stage stops being split around the
partition entry array as a bonus.

Switching the value does not change a file `make` looks at, hence the `rm`.

## Running

| Target | What it does |
| --- | --- |
| `make run` | BIOS, no window, serial to `build/serial.log` |
| `make run-uefi` | the same image under OVMF |
| `make run-gpu` | a window, with virtio-gpu scanout |
| `make headless` | serial on stdin/stdout |

Useful overrides: `QEMU_SMP`, `QEMU_MEMORY`, `QEMU_NET`, `QEMU_AUDIO`.

## Debugging a boot

A panic reaches the screen from `terminal_init()` onwards, which `kmain` calls
right after the heap and well before the root filesystem -- so a machine that
cannot find its root says so on the display, and lists the block devices the
probe did register, instead of going black. It prints the last two dozen lines
of the kernel log above the panic as well, which on a machine with nothing
attached to its serial port is the only way to read them.

The log goes to the console as it is written, too, so a machine that stops
somewhere between the console coming up and init running says which line it got
to instead of leaving a cursor on a black screen. The terminal draws only while
the console owns the framebuffer, so once weston has the display these go to
the serial port and the ring buffer and nowhere else.

```sh
make kernel KERNEL_CFLAGS_EXTRA=-DTUNIX_DEBUG_LOGS=1
```

turns on the kernel's own commentary -- every `fork`, `exec`, `exit` and every
syscall that answered `ENOSYS`. `TUNIX_BOOT_TIMINGS=1` times the boot stages
instead. Both go to the serial log.

To run something other than init, put `init=` on the Limine command line in
`support/limine.conf` and rebuild:

```
cmdline: root=LABEL=tunix-root init=/usr/bin/uname
```

`root=` takes a label or a device name. The image ships with the label, which
is the only form that finds the right disk on a machine that has others;
`root=/dev/sda2` still works and names a position in the probe order.

## How much room the image has

The root filesystem is as big as the tree needs plus `IMAGE_SLACK_MIB`, which
is 4096 -- four gigabytes free on a 4.7 GB image. Half a gigabyte was enough
while "a package" meant a compiler; it is not enough for a Qt application with
a JVM under it that then downloads its own content.

```sh
rm -f build/tunix.img
make image IMAGE_SLACK_MIB=8192
```

Nothing is written that is not used, so the cost is the time to build the image
and to copy it onto a stick.

## The boot menu

Three entries, and the second two exist to take a machine that will not finish
booting apart:

| Entry | Command line | What it separates |
| --- | --- | --- |
| `Tunix` | | |
| `Tunix (one processor)` | `nosmp` | the other processors, and everything they race with |
| `Tunix (shell)` | `init=/bin/sh verbose` | userland working from the boot scripts getting through |

A prompt from the third means the kernel, the disk, the loader and libc are all
fine and what is wrong is above them.

`verbose` prints the first two dozen faults and the first two dozen syscalls
and then goes quiet. It is the only syscall tracing left in the kernel: a
reporter that named every call answered `ENOSYS` or `EOPNOTSUPP` earned its
keep twice -- it is how `TCP_NODELAY` was found -- and then printed seven lines
about `rseq` on every boot forever after, which is not a diagnostic. The
failures that still speak up are the ones that only happen when something is
wrong: a refused socket option, a file that cannot grow, a disk command that
had to be retried, a lock nobody gives back.

The trace looks like this:

```
syscall: 257 from pid 1
fault: pid 1 rip 0x7f0000013134 addr 0x6000001eb0fc error 6
```

That window is the one thing the kernel otherwise has no way to show. A
process that never reaches its first syscall and never takes an unhandled
fault -- which the fault reporter would print on its own -- is, from outside,
a machine that printed its last line and stopped. Whether *any* syscall
appears is the whole question, and the answer separates "userland never ran"
from "userland ran and is stuck in the kernel".

## Booting on one processor

`Tunix (one processor)` is the same image with `nosmp` on the command line. It is for a machine that will not
finish booting: everything the other processors bring with them -- the TLB
shootdown, the contention on the kernel lock, one of them running init while
the first idles -- stops being a suspect.

The kernel says which way it went, and then what it did with init:

```
SMP: 8 of 8 processors running
TUNIX: starting /sbin/init
TUNIX: cpu 0 has nothing to run
```

Init leaving is fatal and says so, because from the outside it is not
distinguishable from a hang -- every processor goes idle and the screen keeps
whatever was on it:

```
TUNIX: init exited, status 0
PANIC: init exited
```

and a fatal signal names where it died:

```
TUNIX: init killed by signal 11 at rip 0x... rsp 0x...
```

The `cpu 0 has nothing to run` line is not a failure. On a machine with several processors another
one takes init before the first gets there, and the first idles; the line
exists because it is the difference between an init running somewhere else and
an init that never ran. On one processor it reads `cpu 0 entering user mode`
instead. The `SMP:` line is printed after the bring-up releases the kernel
lock, so a machine that stops on it rather than after it is one where a
processor took that lock and did not give it back.

## What CI builds

```sh
make check
```

The kernel, three times: as it ships, with `TUNIX_DEBUG_LOGS=1`, and with
`TUNIX_BOOT_TIMINGS=1`. The last two are the point. They wrap code nothing else
refers to, so a change that breaks one of them compiles perfectly well and
stays broken until somebody turns it on to debug something -- which is the
worst moment to find out. `make kernel` passes in that state; `make check` does
not.

`.github/workflows/kernel.yml` runs it on every push and pull request. It does
not build the image: that installs a Void sysroot with xbps, most of a gigabyte
over the network from a mirror this repository does not control, and a build
that fails when somebody else is having a bad day teaches people to ignore it.

## Cleaning

```sh
make clean      # the kernel and the image
make distclean  # everything, including the downloads
```
