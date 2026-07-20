#!/usr/bin/env bash
set -euo pipefail

# Stage the wayland-protocols XML for Tunix builds.
#
# These are the protocol descriptions beyond the core one -- xdg-shell,
# linux-dmabuf, presentation-time and the rest. Weston runs wayland-scanner over
# them to generate C, so this is a **build-time dependency only**: nothing here
# is installed into the image.
#
# That is why the prefix is the sysroot path itself rather than /usr. The
# consumer asks pkg-config for the `pkgdatadir` variable and then opens the XML
# at that path *on the build host*; a /usr prefix would hand weston a path that
# only exists inside Tunix. Ordinary libraries want the /usr prefix (their paths
# are resolved at runtime, on the target) -- this one is the exception.
#
# Upstream declares no language, so nothing is compiled and no toolchain is
# involved.
#
# Output layout:
#   $OUT/graphics-sysroot/usr/share/wayland-protocols   the XML
#   $OUT/graphics-sysroot/usr/share/pkgconfig           wayland-protocols.pc

PORT_NAME=wayland-protocols
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=${OUT:-$ROOT/ports/out}
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)}
GRAPHICS_SYSROOT="$OUT/graphics-sysroot"

SOURCE="$ROOT/ports/src/wayland-protocols"
BUILD="$OUT/wayland-protocols-build"

EXPECTED_VERSION=1.49

fail() {
    echo "build-$PORT_NAME: $*" >&2
    exit 1
}

[[ -f "$SOURCE/meson.build" ]] || fail \
    "missing wayland-protocols source at $SOURCE; run git submodule update --init --recursive"
for tool in meson ninja pkg-config; do
    command -v "$tool" >/dev/null 2>&1 || fail "$tool was not found"
done

version=$(sed -n "s/^[[:space:]]*version:[[:space:]]*'\([0-9.]*\)'.*/\1/p" \
    "$SOURCE/meson.build" | head -n1)
[[ "$version" == "$EXPECTED_VERSION" ]] || \
    fail "expected wayland-protocols $EXPECTED_VERSION, found ${version:-unknown}"

rm -rf "$BUILD"
mkdir -p "$BUILD" "$GRAPHICS_SYSROOT/usr"

# tests off: they need wayland-scanner plus a compiler, and validate upstream's
# XML rather than anything about this port.
meson setup "$BUILD" "$SOURCE" \
    --prefix="$GRAPHICS_SYSROOT/usr" \
    -Dtests=false

meson install -C "$BUILD"

datadir="$GRAPHICS_SYSROOT/usr/share/wayland-protocols"
[[ -f "$datadir/stable/xdg-shell/xdg-shell.xml" ]] || \
    fail "xdg-shell.xml was not installed; weston's shell needs it"
[[ -f "$datadir/stable/presentation-time/presentation-time.xml" ]] || \
    fail "presentation-time.xml was not installed"

pc="$GRAPHICS_SYSROOT/usr/share/pkgconfig/wayland-protocols.pc"
[[ -f "$pc" ]] || fail "wayland-protocols.pc was not installed"

# The whole point of the sysroot prefix: the path the .pc advertises has to
# exist on this machine, because that is where wayland-scanner will read from.
advertised=$(PKG_CONFIG_LIBDIR="$GRAPHICS_SYSROOT/usr/share/pkgconfig" \
    pkg-config --variable=pkgdatadir wayland-protocols)
[[ -d "$advertised" ]] || \
    fail "wayland-protocols.pc advertises '$advertised', which does not exist on the build host"

printf 'wayland-protocols %s staged for the build at %s\n' "$version" "$datadir"
