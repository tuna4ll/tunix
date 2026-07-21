#!/usr/bin/env bash
set -euo pipefail

# Build libseat for Tunix.
#
# Weston 14 has exactly one launcher -- launcher-libseat.c is the only entry in
# ifaces[] in libweston/launcher-util.c -- so there is no way to run the DRM
# backend without libseat. A launcher is what opens /dev/dri and /dev/input on
# the compositor's behalf and hands over the session.
#
# Two things make this tractable on a system with no VT layer and no dbus:
#
#   * the *builtin* backend runs the seatd server in-process, so no daemon and
#     no socket are involved; and
#   * SEATD_VTBOUND=0 turns off VT binding entirely (server.c reads it), which
#     is the only reason the whole VT_SETMODE / KDSETMODE family never gets
#     called. Tunix has no virtual terminals to switch between anyway.
#
# The seatd daemon and the socket backend are therefore both disabled: they
# would only be dead code in the image. logind is disabled for the obvious
# reason.
#
# Output layout:
#   $OUT/graphics-sysroot/usr/{include,lib}   header + .pc for the weston build
#   $OUT/seatd-root/usr/lib                   shared library staged for the image

PORT_NAME=seatd
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=${OUT:-$ROOT/ports/out}

# shellcheck source=ports/lib/cross-port.sh
source "$ROOT/ports/lib/cross-port.sh"

SOURCE="$ROOT/ports/src/seatd"
BUILD="$OUT/seatd-build"
ROOT_DIR="$OUT/seatd-root"
CROSS_FILE="$OUT/tunix-meson-cross.ini"

EXPECTED_VERSION=0.9.3

[[ -f "$SOURCE/meson.build" ]] || cross_port_fail \
    "missing seatd source at $SOURCE; run git submodule update --init --recursive"

cross_port_require_toolchain
cross_port_require_tools meson ninja pkg-config "$READELF"

version=$(sed -n "s/^[[:space:]]*version[[:space:]]*:[[:space:]]*'\([0-9.]*\)'.*/\1/p" \
    "$SOURCE/meson.build" | head -n1)
[[ "$version" == "$EXPECTED_VERSION" ]] || \
    cross_port_fail "expected seatd $EXPECTED_VERSION, found ${version:-unknown}"

# The escape hatch this whole port depends on. If a future version drops it,
# libseat will start issuing VT ioctls Tunix cannot answer, and the failure
# would show up as an unexplained weston startup error rather than here.
grep -q 'SEATD_VTBOUND' "$SOURCE/seatd/server.c" || \
    cross_port_fail "seatd no longer honours SEATD_VTBOUND; VT-less operation needs rechecking"

rm -rf "$BUILD" "$ROOT_DIR"
mkdir -p "$BUILD" "$ROOT_DIR"

cross_port_write_meson_cross "$CROSS_FILE"
cross_port_export_pkg_config

meson setup "$BUILD" "$SOURCE" \
    --cross-file "$CROSS_FILE" \
    --prefix=/usr \
    --libdir=lib \
    --buildtype=release \
    --default-library=shared \
    -Dlibseat-builtin=enabled \
    -Dlibseat-seatd=disabled \
    -Dlibseat-logind=disabled \
    -Dserver=disabled \
    -Dexamples=disabled \
    -Dman-pages=disabled

meson compile -C "$BUILD" -j "$JOBS"

DESTDIR="$GRAPHICS_SYSROOT" meson install -C "$BUILD" --no-rebuild
DESTDIR="$ROOT_DIR" meson install -C "$BUILD" --no-rebuild

[[ -f "$GRAPHICS_SYSROOT/usr/include/libseat.h" ]] || \
    cross_port_fail "libseat.h was not installed into the graphics sysroot"
[[ -f "$GRAPHICS_SYSROOT/usr/lib/pkgconfig/libseat.pc" ]] || \
    cross_port_fail "libseat.pc was not installed into the graphics sysroot"

shared=$(find "$ROOT_DIR/usr/lib" -maxdepth 1 -type f -name 'libseat.so.1*' -print -quit)
[[ -n "$shared" ]] || cross_port_fail "the libseat shared library was not installed"
cross_port_check_library "$shared" libseat.so.1

# Without the builtin backend compiled in, libseat would find no backend at
# runtime and weston would fail with nothing but "Trying libseat launcher...".
# Captured rather than piped into grep -q: under pipefail, grep exiting on the
# first match kills readelf with SIGPIPE and the pipeline "fails" on success.
rodata=$("$READELF" -p .rodata "$shared")
[[ "$rodata" == *builtin* ]] || \
    cross_port_fail "libseat was built without the builtin backend"

rm -rf "$ROOT_DIR/usr/include" "$ROOT_DIR/usr/lib/pkgconfig" "$ROOT_DIR/usr/share" \
       "$ROOT_DIR/usr/bin"
find "$ROOT_DIR/usr/lib" -maxdepth 1 -name '*.a' -delete
find "$ROOT_DIR/usr/lib" -maxdepth 1 -type l -name '*.so' -delete

cross_port_finalize_root "$ROOT_DIR"
cross_port_check_runtime_closure "$ROOT_DIR"

printf 'libseat %s staged at %s\n' "$version" "$ROOT_DIR"
