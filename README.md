# Tunix

Tunix is a very small x86_64 Unix-like operating system.

## Build

Clone or initialize the repository submodules, then build the image:

```sh
git submodule update --init --recursive
make -j"$(nproc)"
```

TinyCC is built automatically from `ports/src/tinycc` and installed into the
Tunix image together with its musl headers, static libraries, startup objects,
and runtime files.
