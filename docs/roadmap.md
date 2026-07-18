# Tunix Roadmap

This roadmap is for practical next steps, not a feature wishlist. Each item
should make Tunix more useful as a small Unix-like system while staying close
to the kernel and userspace that already exist.

## 1. TCP Stack

Tunix already has RTL8139, ARP, IPv4, ICMP, UDP, raw sockets, packet sockets,
and `AF_NETLINK`/rtnetlink with the iproute2 `ip` and `ss` tools. The missing
network primitive is `AF_INET` `SOCK_STREAM`.

First target:

- TCP client sockets: `socket`, `connect`, `send`, `recv`, and `close`.
- Basic TCP state machine: `SYN`, `ESTABLISHED`, `FIN_WAIT`, `CLOSE_WAIT`, and
  `TIME_WAIT`.
- Sequence/ack handling, retransmit timeout, window tracking, and checksum.
- `/proc/net/tcp` entries for live sockets.

Later target:

- Listening sockets with `bind`, `listen`, and `accept`.
- Loopback-friendly behavior for local tests.
- Enough compatibility for simple HTTP clients and small TCP servers.

Done when:

- A tiny Tunix-native TCP client can fetch a plain HTTP response from QEMU user
  networking.
- A small custom tool can open a TCP connection without raw socket
  workarounds.

## 2. Persistent File System

The root filesystem is currently initramfs-backed. That is good for booting,
but it makes `/home`, build output, logs, and edited files temporary.

First target:

- A small read/write filesystem backed by the ATA disk.
- Mount it at `/home` or `/mnt`.
- Support regular files, directories, unlink, rename, truncate, and chmod.
- Add a host-side image builder or formatter.

Later target:

- Crash-tolerant metadata updates.
- A simple `fsck` tool.
- Optional read-only import support for another simple format if useful.

Done when:

- Files created in Tunix survive reboot.
- `nano`, shell history, small compiled binaries, and logs can live outside the
  initramfs.

## 3. GCC Port

Tunix already ships TinyCC, which is useful for small programs and fast checks.
A GCC port is a bigger milestone: it proves that the userspace, headers,
dynamic runtime, filesystem behavior, and process model are strong enough for a
serious compiler toolchain.

First target:

- Build a cross GCC on the host that targets Tunix x86_64 userspace.
- Keep the initial language set to C only.
- Use the existing musl-based sysroot and Tunix headers deliberately, without
  leaking host headers or host libraries.
- Produce dynamically linked binaries using `/lib/ld-musl-x86_64.so.1`.
- Add a small compile/link/run validation program to the image.

Later target:

- Package `gcc`, `cpp`, `as`, `ld`, startup objects, headers, and libraries into
  a coherent `/usr` layout.
- Support building non-trivial C programs inside Tunix once the persistent file
  system exists.
- Evaluate C++ only after the C compiler, linker flow, and runtime behavior are
  stable.

Done when:

- A host-built `x86_64-tunix-gcc` can compile a normal C program that runs in
  Tunix.
- A GCC-built binary can use libc, syscalls, dynamic linking, file I/O, and basic
  process behavior without special-case hacks.

## Not Yet

These are useful eventually, but they should not distract from the roadmap
above:

- SMP.
- USB.
- A full desktop compositor.
- ext4.
- TLS or a full userspace package manager.
- Complex graphics apps 