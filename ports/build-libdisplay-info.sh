#!/usr/bin/env bash
set -euo pipefail

# Build libdisplay-info for Tunix.
#
# Weston's DRM backend requires it unconditionally: it is what turns a
# connector's EDID blob into a monitor name and a mode list. Tunix's DRM device
# reports no EDID at all, so nothing here will ever parse anything real -- but
# backend-drm will not configure without the library present, so it is a
# dependency of the "weston draws on the actual screen" step rather than of any
# feature we use.
#
# The pnp.ids table it compiles in comes from the *host's* hwdata package, which
# is fine: it is a static list of manufacturer codes, not host-specific state.
#
# Output layout:
#   $OUT/graphics-sysroot/usr/{include,lib}   headers + .pc for the weston build
#   $OUT/libdisplay-info-root/usr/lib         shared library staged for the image

PORT_NAME=libdisplay-info
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=${OUT:-$ROOT/ports/out}

# shellcheck source=ports/lib/cross-port.sh
source "$ROOT/ports/lib/cross-port.sh"

SOURCE="$ROOT/ports/src/libdisplay-info"
BUILD="$OUT/libdisplay-info-build"
ROOT_DIR="$OUT/libdisplay-info-root"
CROSS_FILE="$OUT/tunix-meson-cross.ini"

EXPECTED_VERSION=0.2.0

[[ -f "$SOURCE/meson.build" ]] || cross_port_fail \
    "missing libdisplay-info source at $SOURCE; run git submodule update --init --recursive"

cross_port_require_toolchain
cross_port_require_tools meson ninja pkg-config python3 "$READELF"

version=$(sed -n "s/^[[:space:]]*version[[:space:]]*:[[:space:]]*'\([0-9.]*\)'.*/\1/p" \
    "$SOURCE/meson.build" | head -n1)
[[ "$version" == "$EXPECTED_VERSION" ]] || \
    cross_port_fail "expected libdisplay-info $EXPECTED_VERSION, found ${version:-unknown}"

# The pnp.ids path is baked into meson.build with no option to override, so a
# missing hwdata fails deep inside a custom_target with a confusing message.
[[ -f /usr/share/hwdata/pnp.ids ]] || \
    cross_port_fail "/usr/share/hwdata/pnp.ids is missing; install the host's hwdata package"

rm -rf "$BUILD" "$ROOT_DIR"
mkdir -p "$BUILD" "$ROOT_DIR"

cross_port_write_meson_cross "$CROSS_FILE"
cross_port_export_pkg_config

meson setup "$BUILD" "$SOURCE" \
    --cross-file "$CROSS_FILE" \
    --prefix=/usr \
    --libdir=lib \
    --buildtype=release \
    --default-library=shared

meson compile -C "$BUILD" -j "$JOBS"

DESTDIR="$GRAPHICS_SYSROOT" meson install -C "$BUILD" --no-rebuild
DESTDIR="$ROOT_DIR" meson install -C "$BUILD" --no-rebuild

[[ -f "$GRAPHICS_SYSROOT/usr/include/libdisplay-info/edid.h" ]] || \
    cross_port_fail "libdisplay-info headers were not installed into the graphics sysroot"
[[ -f "$GRAPHICS_SYSROOT/usr/lib/pkgconfig/libdisplay-info.pc" ]] || \
    cross_port_fail "libdisplay-info.pc was not installed into the graphics sysroot"

# The real file is versioned by the full project version (libdisplay-info.so.0.2.0)
# while the SONAME follows the *minor* -- soversion is version_minor here, not major.
shared=$(find "$ROOT_DIR/usr/lib" -maxdepth 1 -type f -name 'libdisplay-info.so.0.*' -print -quit)
[[ -n "$shared" ]] || cross_port_fail "the libdisplay-info shared library was not installed"
cross_port_check_library "$shared" libdisplay-info.so.2

# di-edid-decode is a diagnostic tool, not something the compositor needs.
rm -rf "$ROOT_DIR/usr/include" "$ROOT_DIR/usr/lib/pkgconfig" "$ROOT_DIR/usr/share" \
       "$ROOT_DIR/usr/bin"
find "$ROOT_DIR/usr/lib" -maxdepth 1 -name '*.a' -delete
find "$ROOT_DIR/usr/lib" -maxdepth 1 -type l -name '*.so' -delete

cross_port_finalize_root "$ROOT_DIR"
cross_port_check_runtime_closure "$ROOT_DIR"

printf 'libdisplay-info %s staged at %s\n' "$version" "$ROOT_DIR"
