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

## Cleaning

```sh
make clean      # the kernel and the image
make distclean  # everything, including the downloads
```
