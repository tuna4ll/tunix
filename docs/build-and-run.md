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
make image SYSROOT=/var/tmp/tunix-sysroot CACHE=/var/tmp/tunix-cache
```

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

## Running

| Target | What it does |
| --- | --- |
| `make run` | BIOS, no window, serial to `build/serial.log` |
| `make run-uefi` | the same image under OVMF |
| `make run-gpu` | a window, with virtio-gpu scanout |
| `make headless` | serial on stdin/stdout |

Useful overrides: `QEMU_SMP`, `QEMU_MEMORY`, `QEMU_NET`, `QEMU_AUDIO`.

## Debugging a boot

```sh
make kernel KERNEL_CFLAGS_EXTRA=-DTUNIX_DEBUG_LOGS=1
```

turns on the kernel's own commentary -- every `fork`, `exec`, `exit` and every
syscall that answered `ENOSYS`. `TUNIX_BOOT_TIMINGS=1` times the boot stages
instead. Both go to the serial log.

To run something other than init, put `init=` on the Limine command line in
`support/limine.conf` and rebuild:

```
cmdline: root=/dev/sda2 init=/usr/bin/uname
```

## Cleaning

```sh
make clean      # the kernel and the image
make distclean  # everything, including the downloads
```
