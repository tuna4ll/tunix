#!/usr/bin/env bash
set -euo pipefail

# Build libevdev for Tunix.
#
# libevdev wraps the kernel's evdev input protocol -- the /dev/input/event*
# devices Tunix already exposes. Weston 14 depends on it unconditionally, in the
# core rather than in a backend, so it is needed even for a headless compositor
# that will never open an input device.
#
# Tests, tools and documentation are all off: they need `check`, doxygen and a
# runnable target, and none of them ship.
#
# Output layout:
#   $OUT/graphics-sysroot/usr/{include,lib}   headers + .pc for libinput/weston
#   $OUT/libevdev-root/usr/lib                shared library staged for the image

PORT_NAME=libevdev
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=${OUT:-$ROOT/ports/out}

# shellcheck source=ports/lib/cross-port.sh
source "$ROOT/ports/lib/cross-port.sh"

SOURCE="$ROOT/ports/src/libevdev"
BUILD="$OUT/libevdev-build"
ROOT_DIR="$OUT/libevdev-root"
CROSS_FILE="$OUT/tunix-meson-cross.ini"

EXPECTED_VERSION=1.13.6

[[ -f "$SOURCE/meson.build" ]] || cross_port_fail \
    "missing libevdev source at $SOURCE; run git submodule update --init --recursive"

cross_port_require_toolchain
# The event-name tables are generated from the kernel headers by a python script.
cross_port_require_tools meson ninja pkg-config python3 "$READELF"

version=$(sed -n "s/^[[:space:]]*version:[[:space:]]*'\([0-9.]*\)'.*/\1/p" \
    "$SOURCE/meson.build" | head -n1)
[[ "$version" == "$EXPECTED_VERSION" ]] || \
    cross_port_fail "expected libevdev $EXPECTED_VERSION, found ${version:-unknown}"

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
    -Dtests=disabled \
    -Dtools=disabled \
    -Ddocumentation=disabled

meson compile -C "$BUILD" -j "$JOBS"

DESTDIR="$GRAPHICS_SYSROOT" meson install -C "$BUILD" --no-rebuild
DESTDIR="$ROOT_DIR" meson install -C "$BUILD" --no-rebuild

[[ -f "$GRAPHICS_SYSROOT/usr/include/libevdev-1.0/libevdev/libevdev.h" ]] || \
    cross_port_fail "libevdev headers were not installed into the graphics sysroot"
[[ -f "$GRAPHICS_SYSROOT/usr/lib/pkgconfig/libevdev.pc" ]] || \
    cross_port_fail "libevdev.pc was not installed into the graphics sysroot"

shared=$(find "$ROOT_DIR/usr/lib" -maxdepth 1 -type f -name 'libevdev.so.2*' -print -quit)
[[ -n "$shared" ]] || cross_port_fail "libevdev shared library was not installed"
cross_port_check_library "$shared" libevdev.so.2

rm -rf "$ROOT_DIR/usr/include" "$ROOT_DIR/usr/lib/pkgconfig" "$ROOT_DIR/usr/share"
find "$ROOT_DIR/usr/lib" -maxdepth 1 -name '*.a' -delete
find "$ROOT_DIR/usr/lib" -maxdepth 1 -type l -name '*.so' -delete

cross_port_finalize_root "$ROOT_DIR"
cross_port_check_runtime_closure "$ROOT_DIR"

printf 'libevdev %s staged at %s\n' "$version" "$ROOT_DIR"
