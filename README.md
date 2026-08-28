# Tunix

Tunix is a Unix-like operating system for x86_64: a kernel written from scratch,
booted by Limine, running an unmodified Void Linux userland on top of it.

![Weston on Tunix](screenshots/weston.png)

The point of the split is that nothing above the kernel is written here. The
image is Void's own glibc packages, installed by Void's own package manager, on
Void's own init. When one of them does not work, the kernel is wrong -- which is
a much better test than a userland built to suit it.

## Features

- **A monolithic x86_64 kernel**: virtual memory with copy-on-write fork,
  preemptive scheduling across every processor the firmware describes, ELF
  loading with a dynamic linker, signals, and a Linux-compatible syscall table
- **Booted by [Limine](https://github.com/limine-bootloader/limine)**, BIOS or
  UEFI, from one GPT image with an EFI system partition and an ext2 root --
  see [Boot](docs/boot.md)
- **A Void Linux glibc userland**: bash, coreutils, util-linux, iproute2,
  shadow, sudo, curl, nano, htop -- installed with xbps and not built here --
  see [The userland](docs/userland.md)
- **A Wayland desktop**: Void's own Weston, started by runit as an ordinary
  user and given the display by seatd -- see [The desktop](docs/desktop.md)
- **runit as PID 1**, unmodified, with udevd and agetty on the text consoles
- **Storage**: a block layer over IDE, AHCI, NVMe and USB mass storage, GPT and
  MBR partitions, and a read-write ext2 root that `mkfs.ext2` made -- see
  [The root filesystem](docs/filesystem.md)
- **Symmetric multiprocessing**: every processor started and scheduled on, with
  a kernel lock that has a shared mode -- see [Multiprocessor](docs/multiprocessor.md)
- **Networking**: RTL8139, IPv4, ARP, ICMP, UDP, both ends of TCP, netlink and
  a real loopback -- see [Networking](docs/networking.md)
- **A framebuffer console** with eight virtual terminals on Ctrl+Alt+F1..F8,
  and the VT handshake a compositor needs to be moved off one -- see
  [Virtual Terminals](docs/virtual-terminals.md)
- **A virtio-gpu driver**, so the display is scanned out where it lies instead
  of being copied every frame -- see [virtio-gpu](docs/virtio-gpu.md)
- **Intel HD Audio** behind ALSA's `/dev/snd` interface
- **Real users**: per-process credentials, permission checks, setuid binaries,
  and `su`/`sudo` that work because the kernel honours the bit -- see
  [Users and Permissions](docs/users-and-permissions.md)

## Quick start

```sh
make          # kernel, sysroot and image
make run      # boot it
```

The first build downloads a Void rootfs and about 600 MiB of packages. The
sysroot has to be built as root, on a filesystem that can hold ownership and the
setuid bit; see [Build and Run](docs/build-and-run.md) if yours cannot.

It boots into Weston. Ctrl+Alt+F2 leaves it for a text console, where you can
log in as `tunix` / `tunix` or `root` / `tunix`; Ctrl+Alt+F1 goes back.

## Documentation

- [Build and Run](docs/build-and-run.md)
- [Boot](docs/boot.md)
- [The userland](docs/userland.md)
- [The desktop](docs/desktop.md)
- [The root filesystem](docs/filesystem.md)
- [Syscalls and Scheduler](docs/syscalls-and-scheduler.md)
- [Multiprocessor](docs/multiprocessor.md)
- [Memory Layout](docs/memory-layout.md)
- [Mounting](docs/mounting.md)
- [Networking](docs/networking.md)
- [Users and Permissions](docs/users-and-permissions.md)
- [Virtual Terminals](docs/virtual-terminals.md)
- [Power Management](docs/power-management.md)
- [virtio-gpu](docs/virtio-gpu.md)
- [Roadmap](docs/roadmap.md)

## Layout

```
kernel/
  arch/x86_64/  everything that only makes sense on one processor
  drivers/      the hardware: acpi, apic, pci, serial, ata, input, drm,
                and audio/ net/ storage/ usb/ virtio/ under it
  fs/           the VFS and the filesystems on it: ext2, fat, proc, sys, dev
  net/          the stack: IPv4, TCP, UDP, netlink
  ipc/          what a descriptor can be: pipes, unix sockets, epoll, memfd
  tty/          terminals: the line discipline, the screen, the virtual ones
  lib/          the small pieces the freestanding build has to bring itself
  *.c           the core the rest is built on: syscalls, processes, memory
base-files/   what makes the Void sysroot into Tunix
support/      the sysroot and image builders, limine.conf, the font tool
docs/
GNUmakefile   the whole build
```

The build finds sources by walking `kernel/`, so a new file needs no entry
anywhere; only its includes have to know how far down it sits.
