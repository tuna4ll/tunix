#!/usr/bin/env bash
set -euo pipefail

# Build libdrm for Tunix.
#
# libdrm is a thin userspace wrapper around the kernel's DRM ioctl interface in
# <linux/drm.h>.  Tunix does not have a DRM driver yet -- graphics go through
# /dev/fb0 and the TUNIX_FBIO_* ioctls -- so nothing in the image can open a
# /dev/dri node today.  We still build and ship it because:
#
#   * mesa's GBM backend links against it, and GBM is part of the mesa port;
#   * it is the piece that has to be in place before a kernel DRM device is
#     worth writing, and having it built keeps that work to the kernel side.
#
# Only the core library is built: every vendor-specific helper (libdrm_intel,
# libdrm_amdgpu, ...) drives hardware Tunix has no driver for.
#
# Output layout:
#   $OUT/graphics-sysroot/usr/{include,lib}   headers + .pc for the mesa build
#   $OUT/libdrm-root/usr/lib                  shared library staged for the image

PORT_NAME=libdrm
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=${OUT:-$ROOT/ports/out}

# shellcheck source=ports/lib/cross-port.sh
source "$ROOT/ports/lib/cross-port.sh"

SOURCE="$ROOT/ports/src/libdrm"
BUILD="$OUT/libdrm-build"
ROOT_DIR="$OUT/libdrm-root"
CROSS_FILE="$OUT/tunix-meson-cross.ini"

EXPECTED_VERSION=2.4.134

[[ -f "$SOURCE/meson.build" ]] || cross_port_fail \
    "missing libdrm source at $SOURCE; run git submodule update --init --recursive"

cross_port_require_toolchain
cross_port_require_tools meson ninja pkg-config python3 nm "$READELF"

version=$(sed -n "s/^[[:space:]]*version[[:space:]]*:[[:space:]]*'\([0-9.]*\)'.*/\1/p" \
    "$SOURCE/meson.build" | head -n1)
[[ "$version" == "$EXPECTED_VERSION" ]] || \
    cross_port_fail "expected libdrm $EXPECTED_VERSION, found ${version:-unknown}"

rm -rf "$BUILD" "$ROOT_DIR"
mkdir -p "$BUILD" "$ROOT_DIR" "$GRAPHICS_SYSROOT/usr/lib/pkgconfig" \
         "$GRAPHICS_SYSROOT/usr/include"

cross_port_write_meson_cross "$CROSS_FILE"
cross_port_export_pkg_config

meson setup "$BUILD" "$SOURCE" \
    --cross-file "$CROSS_FILE" \
    --prefix=/usr \
    --libdir=lib \
    --buildtype=release \
    --default-library=shared \
    -Dintel=disabled \
    -Dradeon=disabled \
    -Damdgpu=disabled \
    -Dnouveau=disabled \
    -Dvmwgfx=disabled \
    -Domap=disabled \
    -Dexynos=disabled \
    -Dfreedreno=disabled \
    -Dtegra=disabled \
    -Dvc4=disabled \
    -Detnaviv=disabled \
    -Dcairo-tests=disabled \
    -Dman-pages=disabled \
    -Dvalgrind=disabled \
    -Dtests=false \
    -Dinstall-test-programs=false \
    -Dudev=false

meson compile -C "$BUILD" -j "$JOBS"

# Two installs from one build: the sysroot is what mesa compiles against, the
# staged root is what the image ships.
DESTDIR="$GRAPHICS_SYSROOT" meson install -C "$BUILD" --no-rebuild
DESTDIR="$ROOT_DIR" meson install -C "$BUILD" --no-rebuild

[[ -f "$GRAPHICS_SYSROOT/usr/include/xf86drm.h" ]] || \
    cross_port_fail "libdrm headers were not installed into the graphics sysroot"
[[ -f "$GRAPHICS_SYSROOT/usr/include/libdrm/drm.h" ]] || \
    cross_port_fail "libdrm UAPI headers were not installed into the graphics sysroot"
[[ -f "$GRAPHICS_SYSROOT/usr/lib/pkgconfig/libdrm.pc" ]] || \
    cross_port_fail "libdrm.pc was not installed into the graphics sysroot"

shared=$(find "$ROOT_DIR/usr/lib" -maxdepth 1 -type f -name 'libdrm.so.2.*' -print -quit)
[[ -n "$shared" ]] || cross_port_fail "libdrm shared library was not installed"
cross_port_check_library "$shared" libdrm.so.2
[[ -L "$ROOT_DIR/usr/lib/libdrm.so.2" ]] || cross_port_fail "libdrm SONAME symlink is missing"

# The image has no use for headers, static archives or pkg-config data; those
# stay in the sysroot for the mesa build only.
rm -rf "$ROOT_DIR/usr/include" "$ROOT_DIR/usr/lib/pkgconfig" "$ROOT_DIR/usr/share"
find "$ROOT_DIR/usr/lib" -maxdepth 1 -name '*.a' -delete
# libdrm.so (the unversioned linker symlink) is a development artefact too.
rm -f "$ROOT_DIR/usr/lib/libdrm.so"

cross_port_finalize_root "$ROOT_DIR"

printf 'libdrm %s staged at %s\n' "$version" "$ROOT_DIR"
