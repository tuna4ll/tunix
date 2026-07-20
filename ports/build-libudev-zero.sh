#!/usr/bin/env bash
set -euo pipefail

# Build libudev-zero for Tunix.
#
# libinput asks for libudev to enumerate input devices and receive hotplug
# events. Real udev is a systemd component and porting it would be a project of
# its own; libudev-zero is a drop-in replacement written for exactly this
# situation -- it provides the libudev API against plain /dev and /sys, which is
# all a compositor needs.
#
# It is deliberately not a full udev: there is no rules engine and no daemon.
# Device enumeration works, hotplug is limited. For a headless weston none of
# that is exercised at all; for the later DRM backend it is the piece that would
# need attention first.
#
# Plain POSIX Makefile, so the cross build is just CC plus the install paths.
#
# Output layout:
#   $OUT/graphics-sysroot/usr/{include,lib}   headers + .pc for libinput
#   $OUT/libudev-zero-root/usr/lib            libudev.so.1 staged for the image

PORT_NAME=libudev-zero
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=${OUT:-$ROOT/ports/out}

# shellcheck source=ports/lib/cross-port.sh
source "$ROOT/ports/lib/cross-port.sh"

SOURCE="$ROOT/ports/src/libudev-zero"
BUILD="$OUT/libudev-zero-build"
ROOT_DIR="$OUT/libudev-zero-root"

[[ -f "$SOURCE/Makefile" ]] || cross_port_fail \
    "missing libudev-zero source at $SOURCE; run git submodule update --init --recursive"

cross_port_require_toolchain
cross_port_require_tools make "$READELF"

rm -rf "$BUILD" "$ROOT_DIR"
mkdir -p "$BUILD" "$ROOT_DIR"

# The Makefile builds in-tree, so work on a copy and leave ports/src alone.
tar -C "$SOURCE" --exclude=.git -cf - . | tar -C "$BUILD" -xf -

make -C "$BUILD" -j"$JOBS" \
    CC="$CROSS_CC" AR="$CROSS_AR" \
    PREFIX=/usr LIBDIR=/usr/lib

for destination in "$GRAPHICS_SYSROOT" "$ROOT_DIR"; do
    make -C "$BUILD" install \
        PREFIX=/usr LIBDIR=/usr/lib DESTDIR="$destination"
done

[[ -f "$GRAPHICS_SYSROOT/usr/include/libudev.h" ]] || \
    cross_port_fail "libudev.h was not installed into the graphics sysroot"
[[ -f "$GRAPHICS_SYSROOT/usr/lib/pkgconfig/libudev.pc" ]] || \
    cross_port_fail "libudev.pc was not installed into the graphics sysroot"

[[ -f "$ROOT_DIR/usr/lib/libudev.so.1" ]] || \
    cross_port_fail "libudev.so.1 was not installed"
cross_port_check_library "$ROOT_DIR/usr/lib/libudev.so.1" libudev.so.1

rm -rf "$ROOT_DIR/usr/include" "$ROOT_DIR/usr/lib/pkgconfig" "$ROOT_DIR/usr/share"
find "$ROOT_DIR/usr/lib" -maxdepth 1 -name '*.a' -delete
find "$ROOT_DIR/usr/lib" -maxdepth 1 -type l -name '*.so' -delete

cross_port_finalize_root "$ROOT_DIR"
cross_port_check_runtime_closure "$ROOT_DIR"

printf 'libudev-zero staged at %s\n' "$ROOT_DIR"
