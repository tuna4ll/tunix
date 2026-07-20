# Ports

`ports/` contains the build glue for third-party software that is shipped in the Tunix initramfs. It is not a package manager. Each port is a small script that builds an upstream source tree into `ports/out/`, and the top-level `Makefile` copies the staged files into `build/rootfs/`.

## Layout

```text
ports/
  build-*.sh          Per-port build scripts.
  compat/             Small Tunix-specific compatibility headers.
  out/                Generated output. Removed by `make clean`.
  src/                Third-party source trees, usually Git submodules.
  terminfo/           Terminal descriptions used by ncurses apps.
```

Keep upstream code under `ports/src/` as clean as possible. Tunix-specific changes should usually live in the build script, a wrapper under `tools/`, or a small compatibility header under `ports/compat/`.

## Build Model

Most base userland tools are built as static musl binaries so the system can boot without needing the dynamic loader. Some runtime checks and libraries are also built dynamically to exercise Tunix dynamic ELF support.

The normal entry point is the root `Makefile`:

```sh
make all
make run
make headless
```

Port scripts can also be run directly:

```sh
OUT="$PWD/ports/out" ./ports/build-<name>.sh
```

Use `JOBS` to control parallel builds:

```sh
JOBS=8 make all
```

## Output Rules

Generated files should stay under `ports/out/`. Common patterns are:

```text
ports/out/<name>              Single binary copied by the Makefile.
ports/out/<name>-root/        Staged root tree copied into build/rootfs/.
ports/out/<name>-build/       Disposable build directory.
ports/out/sysroot/            Static musl sysroot shared by static ports.
ports/out/desktop-sysroot/    Shared-library sysroot for dynamic/desktop libs.
ports/out/musl-cross/         x86_64-linux-musl cross toolchain (gcc, g++).
ports/out/graphics-sysroot/   Sysroot for the cross-built graphics ports.
```

## Graphics Stack

`libdrm` and `mesa` do not use the `musl-gcc` wrapper the other ports use. Mesa
is a C++17 project, and the host `libstdc++` is configured for glibc, so it
cannot be compiled against a musl sysroot at all. `ports/build-musl-cross.sh`
therefore builds a real `x86_64-linux-musl` toolchain, with its own
`libstdc++`, via the `musl-cross-make` submodule. It downloads the upstream
gcc, binutils and musl tarballs on the first build and caches them in
`ports/out/musl-cross-dl/`.

That build is the slowest one in the tree. On a working copy that lives on a
Windows drive under WSL, point its scratch directory at a native filesystem:

```sh
MUSL_CROSS_BUILD_DIR=/var/tmp/tunix-musl-cross make
```

Note that `make clean` removes `ports/out/` wholesale, which includes both the
toolchain and its download cache, so the next build pays for the gcc build and
the downloads again. Delete the individual `-root`/`-build` directories instead
when you only want to rebuild a port.

Those two ports also get their own sysroot, `ports/out/graphics-sysroot/`,
rather than sharing `desktop-sysroot/`. The desktop sysroot is a copy of the
`musl-gcc` sysroot, so it carries a second libc; putting it on the cross
compiler's search path would shadow the toolchain's own musl.

Mesa is configured for software rendering with no window system:

```text
gallium softpipe     CPU rasteriser; llvmpipe would need a musl LLVM.
EGL surfaceless      No window system and no DRM device needed.
GLES2                GLX needs X11, so libEGL/libGLESv2 are the client APIs.
GBM                  Built and shipped, but inert until the kernel has DRM.
```

Nothing on Tunix can open a `/dev/dri` node yet, so `libdrm` and GBM are
present for a future kernel DRM driver to plug into. What does work today is
`tunix-gl-demo`, which renders through EGL into a framebuffer object and blits
the result to `/dev/fb0`:

```sh
make gl-check          # renders one frame on the build host
/usr/bin/tunix-gl-demo # renders to the framebuffer inside Tunix
```

Do not make a port install directly into `initrd/` or `build/rootfs/`. The Makefile owns final rootfs assembly.

## How to Port

1. Add or initialize the upstream source under `ports/src/<name>`.
2. Create `ports/build-<name>.sh`.
3. Make the script fail early when required source files or host tools are missing.
4. Build into `ports/out/<name>-build/`, not inside the source checkout.
5. Stage installable files into `ports/out/<name>-root/` or copy a single binary to `ports/out/<name>`.
6. Prefer static musl builds for core command-line tools.
7. Use the shared musl or desktop sysroot only when testing or shipping dynamic libraries is the point of the port.
8. Add cheap validation: version check, expected file check, `readelf` check, or a tiny self-test.
9. Wire the output into the top-level `Makefile`.
10. Add rootfs assertions for files that must exist in the final image.

For static binaries, verify there is no dynamic interpreter and no shared dependency list:

```sh
readelf -l ports/out/<name>
readelf -d ports/out/<name>
```

For dynamic binaries, verify the interpreter path is the Tunix runtime path:

```sh
readelf -l ports/out/<name>
```

Expected interpreter:

```text
/lib/ld-musl-x86_64.so.1
```

## Common Fixes

Initialize missing third-party sources:

```sh
git submodule update --init --recursive
```

Clear stale generated files:

```sh
make clean
make all
```

If a build accidentally finds host headers or host libraries, fix the port script with explicit `CC`, `CPPFLAGS`, `LDFLAGS`, `PKG_CONFIG_PATH`, `PKG_CONFIG_LIBDIR`, or a CMake toolchain file instead of patching upstream source code.
