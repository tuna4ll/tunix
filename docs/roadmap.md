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

## 3. GCC Port — done

GCC 14.2.1 compiles and links on the machine, and `cc` is it. TinyCC has been
removed. It did not arrive the way this section expected: rather than building a
cross GCC on the host, the port *fetches* Void's `x86_64-musl` gcc with xbps and
stages it under `/opt/gcc` — Tunix's musl is the same version as Void's, so the
compiler Void already publishes for this triple is the compiler we want. See
[Ports](ports.md).

What it proved is what mattered: a 40 MiB C compiler runs, spawns cc1 and
collect2, and produces static *and* dynamic binaries against the image's own
headers and libc, with no special-case hacks.

Still open:

- C++ (`cc1plus`) and `-flto` are deliberately not shipped.
- Building non-trivial programs on the machine — the compiler works, the
  question is now the rest of the userland around it.

## Not Yet

These are useful eventually, but they should not distract from the roadmap
above:

- SMP.
- USB.
- A full desktop compositor.
- ext4.
- TLS or a full userspace package manager.
- Complex graphics apps 