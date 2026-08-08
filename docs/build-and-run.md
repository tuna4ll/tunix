# Build and Run

This document covers the normal local workflow for building and booting Tunix.

## Requirements

Tunix is built with the top-level `Makefile`. The build expects a Unix-like
toolchain environment with:

```text
gcc
ld
nasm
strip
ar
python3
make
tar
qemu-system-x86_64
```

Some ports also use common build tools such as `cmake`, `autoreconf`,
`pkg-config`, and `readelf`.

Initialize third-party sources before the first full build:

```sh
git submodule update --init --recursive
```

## Build

Build the disk image:

```sh
make all
```

The main outputs are:

```text
build/tunix.img       Bootable disk image.
build/kernel.elf      Kernel ELF.
build/initramfs.img   Initramfs archive.
build/rootfs/         Temporary root filesystem staging tree.
ports/out/            Built third-party tools and libraries.
```

`make all` may take a while on the first run because it builds the bundled
ports. Later builds reuse `ports/out/` unless it is cleaned.

## Run

Run with the normal QEMU display:

```sh
make run
```

Run without a graphical QEMU window:

```sh
make headless
```

Run with a virtio-gpu instead of the emulated VGA adapter, which is what the
kernel's virtio-gpu driver needs to find — see [virtio-gpu](virtio-gpu.md):

```sh
make run-gpu
```

All three boot `build/tunix.img` with 128 MiB of RAM and an RTL8139 network
device. The graphical run writes serial output to:

```text
build/serial.log
```

## Clean

Remove generated kernel, image, rootfs, and port output:

```sh
make clean
```

This deletes:

```text
build/
ports/out/
```

Use this when a port has stale configure output or a generated file no longer
matches the current source.

## Common Problems

Missing source under `ports/src/` usually means submodules were not initialized:

```sh
git submodule update --init --recursive
```

Missing host tools should be fixed in the host environment, not by editing the
generated output tree.

If a port keeps using old generated files, clean and rebuild:

```sh
make clean
make all
```
