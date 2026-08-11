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

## The Compiler on the Image

Tunix carries two toolchain pieces: binutils (`as`, `ld`, `ar`, …) and **GCC**,
which is what `cc` points at. TinyCC used to be here as well and was removed
once GCC worked; the musl headers and static libc it happened to stage into
`/usr/include` and `/usr/lib` are staged by the GCC port now.

GCC is the one port that is not built here. `ports/build-gcc.sh` *fetches* it,
as a binary package, from Void's `x86_64-musl` repository using our own xbps —
the same route [docs/package-manager.md](package-manager.md) describes, run on
the build host instead of on the machine. Building it instead would mean a
Canadian cross of the largest source tree there is, to produce a compiler Void
already publishes for this exact triple.

Void's gcc is dynamically linked and its libraries share names with the image's
own, so it is staged under a private prefix rather than merged into `/usr`:

```text
/opt/gcc/usr/bin/gcc                  the driver, as Void built it
/opt/gcc/usr/lib/gcc/<triple>/<ver>/  cc1, collect2, crt*.o, libgcc.a
/opt/gcc/usr/lib/*.so.*               only the shared objects it needs
/usr/bin/gcc                          wrapper: sets LD_LIBRARY_PATH, execs it
/usr/bin/cc                           symlink to the wrapper
```

GCC locates cc1 and libgcc relative to its own `argv[0]`, so the prefix moves
with it, but the *system* header and library directories are not relocated —
`/usr/include` and `/usr/lib` stay the image's own musl. Void supplies the
compiler, Tunix supplies the libc, and there is exactly one set of headers.

Things worth knowing about the result:

- **C only.** `cc1plus`, `lto1` and `gnat1` are dropped; they are 130 MiB of
  initramfs, which is loaded into RAM whole. `liblto_plugin.so` stays, because
  this driver passes `-plugin` to the linker on every link and refuses to link
  without it — the image's binutils accepts the option and ignores it.
- **Void's gcc needs a `lib64` exec prefix.** The package installs into `lib`
  and leaves the symlink to `base-files`, which is not one of its dependencies,
  so the port creates it. Without it the driver reports `cannot execute 'cc1'`.
- **Two linker names were missing from the image** and the port adds them:
  `/usr/lib/libc.so` (to the shared musl, which lives in `/lib`) and
  `/usr/lib/libgcc_s.so`. Nothing had ever been linked *on* the machine before,
  so only the SONAMEs existed.
- Both static and dynamic links work. `-flto` and C++ do not.
- **The port also stages the C library it compiles against**: `/usr/include` and
  `/usr/lib` come from the static musl sysroot the binutils port builds, which
  is the same libc every static port here was compiled against.

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

## Wayland

`libffi` and `libwayland` are the first two ports on the way to Weston. They
use the same cross toolchain and graphics sysroot as libdrm and mesa.

`wayland-scanner` is a build-time code generator, so the port does not build it
for the target. In a cross build meson resolves it from the **host** and
requires the version to match the library being built exactly, because the C
glue it generates is compiled into that library:

```sh
pacman -S wayland      # provides a host wayland-scanner
```

`/usr/bin/wayland-roundtrip-test` is the end-to-end check. It runs a real
server and a real client in one process tree and exercises the pieces the
kernel work was for: a listening unix socket, a `wl_display_roundtrip` (libffi
dispatching a marshalled call), and a `wl_shm` pool whose `memfd` travels over
`SCM_RIGHTS` and is mapped by the server.

One trap worth knowing about on a Windows working copy: libffi's
`.gitattributes` marks everything `text=auto`, so a checkout rewrites its shell
scripts with CRLF line endings. `configure` then fails to source
`configure.host`, which is not fatal — it silently leaves the architecture
undetected and the build dies much later. Set `core.eol=lf` and re-check out
the submodule; `ports/build-libffi.sh` detects the symptom and says so. nettle
has the same `.gitattributes` habit, where it surfaces as `./.bootstrap:
/bin/sh^M: bad interpreter`; `ports/build-nettle.sh` checks for it up front.

Do not make a port install directly into `initrd/` or `build/rootfs/`. The Makefile owns final rootfs assembly.

## TLS

GIO does not implement TLS. `g_tls_backend_get_default()` returns whatever
registered itself at the `gio-tls-backend` extension point, and nothing does
until a module in `/usr/lib/gio/modules` is loaded. With that directory empty
libsoup answers every `https://` with "TLS support is not available" and
WebKit fails the load before a packet leaves the machine — a failure that
looks nothing like its cause.

Four ports fill it in, and they must be built in this order:

| Port | What it is |
| --- | --- |
| `gmp` | bignums, from a release tarball — GMP's upstream is Mercurial |
| `nettle` | ciphers, hashes and (as `hogweed`) the public-key half |
| `gnutls` | the TLS implementation, from a release tarball |
| `glib-networking` | the module that registers gnutls as GIO's backend |

Certificates come from `/etc/ssl/cert.pem`, the bundle the image already ships
for mbedTLS, compiled into gnutls as its default trust store. `https-get`,
`curl` and the browser therefore agree on which roots are trusted.

`curl` is a second TLS client alongside all of that, and it does not use
gnutls: `ports/build-curl.sh` builds one static musl tree against mbedTLS and
the image takes both halves of it — the archive git's `git-remote-https` links
against, and `/usr/bin/curl` for the shell. Only the binary is copied in; the
headers and the archive would be dead weight next to a git that is already
linked. The build is deliberately narrow — http, https and file, IPv4 only, no
zlib, no threaded resolver — because a client-side IPv4 stack is all the kernel
offers and anything else would only fail at runtime. The script refuses to
finish if `curl_config.h` does not define `USE_MBEDTLS`, since an SSL-less
libcurl builds cleanly and its only symptom is `Protocol https not supported`
much later.

Name resolution goes through `getaddrinfo()` and works: `curl http://example.com`
resolves, connects and returns a body. An older comment in `tools/https-get.c`
claimed otherwise and was stale — that tool parses A records by hand because it
was written before the resolver worked, not because it has to.

Two checks guard the seam, because "installed" and "loaded" are different
claims. `ports/build-glib-networking.sh` runs `tools/gio-tls-test.c` against
the module it just staged, under the cross loader, and fails if GIO does not
come back with a backend. `ports/build-libsoup.sh` then asks the same question
through libsoup's own `tls_check`. Both need `GIO_MODULE_DIR` pointed at the
sysroot: the directory compiled into `libgio` names the *guest*
`/usr/lib/gio/modules`, which on the build host is the host's own, full of
glibc modules a musl process cannot load.

On the guest, `gio-tls-check https://example.com` does the whole thing for
real — handshake, protocol version, ciphersuite and the HTTP status line.

## Init (dinit)

`ports/build-dinit.sh` builds dinit with the cross toolchain and links it
**statically**: PID 1 must never fail to boot because the dynamic loader or
`libstdc++.so` went missing from the image, and a static musl binary can even
be smoke-tested on the build host (same syscall ABI, no loader). The Makefile
symlinks `/sbin/init` to it; the boot services live in `initrd/etc/dinit.d`
(`rcS` for filesystem setup, `keymap`, and `startx`, which is what brings up
weston at boot) with the global service environment in
`initrd/etc/dinit/environment`.

The one patch tolerates `chmod()` on the control socket failing with `ENOENT`:
Tunix binds unix sockets into a kernel table without creating a filesystem
node, and dinit otherwise treats that as a fatal startup error.

dinit is also why the kernel grew `waitid(2)`, the `chown` family, and real
`EPOLLONESHOT` disarming — its event library (dasynq) reaps children with
`waitid`, `fchown()`s every logfile, and "disables" epoll watches by re-arming
them with only `EPOLLONESHOT` set, expecting them to fall silent afterwards.

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

## Accounts and Privilege

`shadow` and `sudo` are static musl ports like the rest of the base userland,
with two things worth knowing.

shadow files its account tools (`passwd`, `chage`, `useradd`, …) under directory
variables automake does not recognise, so `make install-exec` -- what
`gnu_autotools_port` runs -- installs `login`, `su` and `groups` and silently
skips everything else. `ports/build-shadow.sh` stages the rest explicitly.

sudo writes its own makefiles rather than using automake, so it has no
`install-exec` target at all and does not go through the shared helper. Its
policy must be linked into the binary (`--enable-static-sudoers`), because a
loadable plugin would need a dynamic loader that a static image does not have,
and PIE has to be off: the hardening flags end in `-fPIE -pie`, which cannot
link against the static non-PIE musl.

Neither port can carry its own setuid bit. The staging tree lives on a drive
that reports every file as 0777, so the modes come from
`scripts/rootfs-permissions.conf` when the image is built -- that file, not the
port script, is where `/bin/su` and `/usr/bin/sudo` become 4755.

## Authentication and the Display Manager

Linux-PAM, LightDM and lightdm-gtk-greeter are cross ports on the graphics
sysroot. What they run into is described in
[Display Manager](display-manager.md); what is worth knowing to *build* them:

**PAM cannot be bootstrapped in place on a Windows working copy.** Its
top-level `Makefile.am` pins `AUTOMAKE_OPTIONS = ... gnu ...`, which requires a
`ChangeLog`, and upstream's `autogen.sh` satisfies that by touching one next to
the tracked `CHANGELOG`. On a case-insensitive drive those are the same file,
and automake reads the directory listing itself, so the requirement can never be
met — and the command line cannot lower the strictness, because `Makefile.am`
outranks it. `ports/build-linux-pam.sh` copies the tree to `/var/tmp` and builds
there; `LINUX_PAM_WORK` overrides where.

**LightDM's generated headers only exist in maintainer mode.**
lightdm-gtk-greeter turns its glade file and two stylesheets into C headers with
`xdt-csource`, inside `if MAINTAINER_MODE`. A release tarball ships the results;
a git checkout does not, so the port configures with `--enable-maintainer-mode`.
It also builds in-tree: the sources include `"src/lightdm-gtk-greeter-ui.h"` but
the compile line carries `-I$(top_srcdir)` without `-I$(top_builddir)`, so a
separate object directory never finds them.

**Three patches, all musl or missing-dependency.** libxklavier is dropped from
liblightdm-gobject (and from its `.pc`, or the greeter's configure fails on a
dependency the library no longer uses); the glibc-only `LC_IDENTIFICATION` and
`_NL_IDENTIFICATION_*` lookups fall back to upstream's own path; and
`session-child.c`'s private `updwtmpx` is renamed, because musl provides one and
the two declarations conflict.

## Python

CPython is a cross port on the graphics sysroot, built **shared**. That is the
whole design decision: a static build is far easier to produce, but it can never
`dlopen` anything, which costs `ctypes` and every third-party C extension — most
of the reason to have Python at all.

Two things make cross-building CPython different from the other ports.

**It needs a working Python of the same series to build.** The freeze and
bytecode steps run on the build machine, and the marshalled bytecode format is
version-specific, so `--with-build-python` will not accept a different
major.minor. The port pins 3.14.6 and checks the host's `python3` before it
starts, because the alternative is a failure a long way into the build.

**`--host` puts autoconf in cross mode**, where it cannot run its probe programs
and falls back to guesses. Three of those guesses are wrong for Tunix and are
overridden through a `config.site`: it does have `/dev/ptmx`, it does not have
`/dev/ptc`, and its `getaddrinfo` is fine (the "buggy" default assumes the worst
and disables IPv6 name lookups).

What is deliberately missing, and why:

| Module | Reason |
| --- | --- |
| `_ssl`, `_hashlib` | no OpenSSL; Tunix's TLS stack is GnuTLS. `hashlib` uses CPython's own md5/sha1/sha2/sha3/blake2 |
| `readline`, `_curses` | ncurses is a static port and not in the graphics sysroot. 3.13 onwards has PyREPL, which needs neither |
| `tkinter`, `idlelib` | no Tcl/Tk |
| `ensurepip` | pip cannot fetch anything without `_ssl` |

`Lib/test` and the `config-*` build makefiles are pruned: the test suite is a
third of the tree and nothing on the image runs it, and there is no compiler on
the image that could build an extension against those makefiles.

`/bin/python-test` is the on-image check. It is not a language test — it
exercises the places Python leans on the kernel (threads, `fork`/`exec`/pipes,
signal delivery to a Python handler, unix sockets, `epoll`, `mmap`, `dlopen`
through ctypes, SQLite on disk), so a pass says the syscall surface holds.

### What it found

The port went in cleanly; running it did not. Five things had to be fixed
before `python-test` passed 14/14, and none of them were Python's fault:

| Symptom | Cause |
| --- | --- |
| `FileNotFoundError: '/bin/echo'` | the userland installs into `/usr/bin` and `/bin` held almost nothing. LightDM had already hit this shelling out to `/bin/rm` |
| `accept()` raised `BlockingIOError` | `accept` answered `EAGAIN` whether or not the socket was non-blocking. X11, Wayland and D-Bus poll before accepting, so a plain blocking accept had never been tried |
| SQLite: "disk I/O error" on every file database | `fcntl` did not implement `F_SETLK`. SQLite locks byte ranges to arbitrate between connections and treats the failure as an I/O error |
| writes through a shared `mmap` vanished | `MAP_SHARED` on a file fell through to private pages, and `msync` was a no-op. Writes were discarded silently |
| `settimeout()` raised `EADDRNOTAVAIL` | `FIONBIO` was not handled, so every socket ioctl fell through to the *interface* ioctls, which read the argument as an interface name |

The `mmap` fix had a second half worth knowing about. Mapping the cached pages
only works when they start on a page boundary, and the heap page-aligns
allocations of 64 KiB and up — so the first fix made shared mappings work for
large files and silently not for small ones, which is worse than failing
uniformly. `vfs_align_data()` moves a file's cache onto a page boundary when
something maps it.

### Known deviations

`bind()` on an AF_UNIX socket does not create a socket file. The name lives in
the kernel's own table, which is why `/tmp/.X11-unix/X0` has always worked, but
`stat()` and `unlink()` on a bound path answer `ENOENT`. Programs that clear a
stale socket before binding get an error they normally ignore.

POSIX advisory locks are held at whole-file granularity rather than per byte
range. Locking more than was asked can only refuse a lock that should have been
granted, never grant one that should have been refused, so the direction is
safe; what it costs is concurrency between processes sharing one database.
Within a single process nothing is lost.

A shared file mapping is coherent with `read()` immediately, but reaching the
disk still takes an `fsync` on the descriptor.

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
