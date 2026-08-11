# Tunix

Tunix is a small Unix-like operating system experiment for x86_64. It includes a custom bootloader, kernel, initramfs-based userspace, framebuffer terminal, and a small set of ported userland tools.

![Tunix running the Xfce desktop](screenshots/screenshot.png)

## Features

- Custom bootloader and kernel code
- Persistent ext2 root filesystem on the boot disk (Linux-mountable); the
  initramfs only seeds it on first boot
- `mount`/`umount`: a real mount table, tmpfs mountable anywhere, bind mounts,
  and a `/proc/mounts` that reports it — see [Mounting](docs/mounting.md)
- Hard links: `link()`/`linkat()`, `st_nlink`, and a file that outlives the
  name it was created under — `link-test` checks it on both filesystems
- Framebuffer terminal with keyboard input
- Basic VFS, devfs, procfs, process, and syscall support
- Symmetric multiprocessing: every processor the firmware describes is started
  and the scheduler runs processes on all of them — see
  [Multiprocessor](docs/multiprocessor.md)
- GNU userland (coreutils, grep, sed, gawk, findutils, diffutils, tar, gzip, make), Bash, GCC, binutils, nano, Lua, and selected libraries
- CPython 3.14, built shared so `ctypes` and C extensions load; `python-test`
  exercises threads, subprocess, signals, sockets, epoll, mmap and sqlite3
- Networking with both ends of TCP: `bind`/`listen`/`accept` as well as
  `connect`, and a real loopback, so a server and its client can both run on
  this machine — `tcp-test` proves it over `127.0.0.1`; see
  [Networking](docs/networking.md)
- `curl`, and Git's `https://` transport, both out of one static curl port
  built against mbedTLS — so `curl https://…` and `git clone https://…` work.
  The tool speaks http, https and file over IPv4; `ssh://` remotes are not
  supported
- Intel HD Audio playback behind ALSA's `/dev/snd` interface, with alsa-lib
  ported on top of it: `snd-test` drives the kernel ioctls directly and
  `alsa-test` goes through the library, mixer included
- dinit as PID 1: services under `/etc/dinit.d`, controlled with `dinitctl`
- Real users: per-process credentials, file permission checks, setuid binaries,
  and a console `login` prompt. shadow-utils (`login`, `su`, `passwd`) and
  `sudo` are ported. The image ships `root` / `root` and `tunix` / `tunix` —
  see [Users and Permissions](docs/users-and-permissions.md)
- A full Xfce desktop on Xorg (xfwm4, xfce4-panel, xfdesktop, Thunar and
  xfce4-terminal), started by the session of whoever logs in, so it runs as
  that user; a Weston (Wayland) session is also available
- A graphical login: LightDM with lightdm-gtk-greeter owns the display from
  boot, authenticating through Linux-PAM — see
  [Display Manager](docs/display-manager.md)
- A package manager: xbps, installing from Tunix's own binary repository
  ([tunix-ports](https://github.com/tunixos/tunix-ports), built and published by
  CI) with Void Linux's `x86_64-musl` set behind it — `xbps-install -S && xbps-install -y tty-clock` — see
  [Package Manager](docs/package-manager.md)
- A virtio-gpu driver, so the display is scanned out where it lies instead of
  being copied into the framebuffer every frame; `make run-gpu` — see
  [virtio-gpu](docs/virtio-gpu.md)

## Quick Start

Initialize submodules first:

```sh
git submodule update --init --recursive
```

Build the disk image:

```sh
make all
```

Run it in QEMU:

```sh
make run
```

Or run headless:

```sh
make headless
```

Clean generated files:

```sh
make clean
```

## Documentation

- [Build and Run](docs/build-and-run.md)
- [Ports](docs/ports.md)
- [Syscalls and Scheduler](docs/syscalls-and-scheduler.md)
- [Multiprocessor](docs/multiprocessor.md)
- [Memory Layout](docs/memory-layout.md)
- [Persistent Filesystem](docs/persistent-filesystem.md)
- [Mounting](docs/mounting.md)
- [Networking](docs/networking.md)
- [Users and Permissions](docs/users-and-permissions.md)
- [Display Manager](docs/display-manager.md)
- [virtio-gpu](docs/virtio-gpu.md)
- [Package Manager](docs/package-manager.md)
