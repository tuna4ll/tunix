#!/usr/bin/env bash
set -euo pipefail

# Build libinput for Tunix.
#
# The last dependency weston needs before it can be built. libinput turns raw
# evdev events into the gestures and pointer motion a compositor consumes, on
# top of libevdev and libudev.
#
# Weston 14 requires it in its core rather than in a backend, so it is needed
# even for a headless compositor that opens no input device -- which is the one
# thing this port cannot verify at runtime here, since there is nothing to feed
# it.
#
# Off: libwacom (tablet database, another dependency chain), mtdev (an
# out-of-tree multitouch shim for kernels older than anything Tunix targets),
# the debug GUI (GTK) and the tests (they need a running seat).
#
# Output layout:
#   $OUT/graphics-sysroot/usr/{include,lib}   headers + .pc for weston
#   $OUT/libinput-root/usr/lib                shared library staged for the image

PORT_NAME=libinput
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=${OUT:-$ROOT/ports/out}

# shellcheck source=ports/lib/cross-port.sh
source "$ROOT/ports/lib/cross-port.sh"

SOURCE="$ROOT/ports/src/libinput"
BUILD="$OUT/libinput-build"
ROOT_DIR="$OUT/libinput-root"
CROSS_FILE="$OUT/tunix-meson-cross.ini"

EXPECTED_VERSION=1.29.2

[[ -f "$SOURCE/meson.build" ]] || cross_port_fail \
    "missing libinput source at $SOURCE; run git submodule update --init --recursive"

cross_port_require_toolchain
cross_port_require_tools meson ninja pkg-config python3 "$READELF"

# Upstream writes "version : '1.29.2'" with a space before the colon, so the
# pattern has to tolerate it.
version=$(sed -n "s/^[[:space:]]*version[[:space:]]*:[[:space:]]*'\([0-9.]*\)'.*/\1/p" \
    "$SOURCE/meson.build" | head -n1)
[[ "$version" == "$EXPECTED_VERSION" ]] || \
    cross_port_fail "expected libinput $EXPECTED_VERSION, found ${version:-unknown}"

for module in libevdev libudev; do
    [[ -f "$GRAPHICS_SYSROOT/usr/lib/pkgconfig/$module.pc" ]] || cross_port_fail \
        "$module is not in the graphics sysroot; build it first"
done

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
    -Dlibwacom=false \
    -Dmtdev=false \
    -Ddebug-gui=false \
    -Dtests=false \
    -Ddocumentation=false \
    -Dudev-dir=/usr/lib/udev

meson compile -C "$BUILD" -j "$JOBS"

DESTDIR="$GRAPHICS_SYSROOT" meson install -C "$BUILD" --no-rebuild
DESTDIR="$ROOT_DIR" meson install -C "$BUILD" --no-rebuild

[[ -f "$GRAPHICS_SYSROOT/usr/include/libinput.h" ]] || \
    cross_port_fail "libinput headers were not installed into the graphics sysroot"
[[ -f "$GRAPHICS_SYSROOT/usr/lib/pkgconfig/libinput.pc" ]] || \
    cross_port_fail "libinput.pc was not installed into the graphics sysroot"

shared=$(find "$ROOT_DIR/usr/lib" -maxdepth 1 -type f -name 'libinput.so.10*' -print -quit)
[[ -n "$shared" ]] || cross_port_fail "libinput shared library was not installed"
cross_port_check_library "$shared" libinput.so.10

rm -rf "$ROOT_DIR/usr/include" "$ROOT_DIR/usr/lib/pkgconfig" "$ROOT_DIR/usr/share" \
       "$ROOT_DIR/usr/bin" "$ROOT_DIR/usr/libexec"
find "$ROOT_DIR/usr/lib" -maxdepth 1 -name '*.a' -delete
find "$ROOT_DIR/usr/lib" -maxdepth 1 -type l -name '*.so' -delete

cross_port_finalize_root "$ROOT_DIR"
cross_port_check_runtime_closure "$ROOT_DIR" "$OUT/libevdev-root" "$OUT/libudev-zero-root"

printf 'libinput %s staged at %s\n' "$version" "$ROOT_DIR"
