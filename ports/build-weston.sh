#!/usr/bin/env bash
set -euo pipefail

# Build Weston for Tunix -- headless backend, software renderer.
#
# This is the milestone the whole Wayland chain was for: a real compositor,
# running on Tunix, with no graphics hardware in the picture. The headless
# backend renders into memory and presents nowhere, which is exactly right for
# proving that the *system* works -- unix sockets, SCM_RIGHTS, memfd, epoll,
# signalfd, flock -- before a line of DRM driver exists.
#
# Everything that needs a display, a GPU or a session is off:
#
#   backend-drm      needs /dev/dri, which Tunix does not have yet
#   renderer-gl      weston's pixman renderer composites on the CPU instead
#   xwayland         an X server is a project of its own
#   launcher-libseat seat management; headless opens no privileged device
#   image-jpeg/webp  decoders for backgrounds we do not ship
#   demo-clients     they need cairo
#
# The tests are also off: they assume a full session and a runnable target.
#
# Output layout:
#   $OUT/weston-root/usr/bin/weston            the compositor
#   $OUT/weston-root/usr/lib/{libweston-*,weston}  its modules
#   $OUT/weston-root/usr/share/weston          shell data

PORT_NAME=weston
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=${OUT:-$ROOT/ports/out}

# shellcheck source=ports/lib/cross-port.sh
source "$ROOT/ports/lib/cross-port.sh"

SOURCE="$ROOT/ports/src/weston"
BUILD="$OUT/weston-build"
ROOT_DIR="$OUT/weston-root"
CROSS_FILE="$OUT/tunix-meson-cross.ini"
PATCH_DIR="$ROOT/ports/src/patches/weston"

EXPECTED_VERSION=14.0.3

[[ -f "$SOURCE/meson.build" ]] || cross_port_fail \
    "missing weston source at $SOURCE; run git submodule update --init --recursive"

cross_port_require_toolchain
cross_port_require_tools meson ninja pkg-config python3 "$READELF"

version=$(sed -n "s/^[[:space:]]*version[[:space:]]*:[[:space:]]*'\([0-9.]*\)'.*/\1/p" \
    "$SOURCE/meson.build" | head -n1)
[[ "$version" == "$EXPECTED_VERSION" ]] || \
    cross_port_fail "expected weston $EXPECTED_VERSION, found ${version:-unknown}"

# Weston's core requires all of these regardless of which backend is enabled.
for module in wayland-server wayland-client pixman-1 xkbcommon libevdev libinput libdrm; do
    [[ -f "$GRAPHICS_SYSROOT/usr/lib/pkgconfig/$module.pc" ]] || cross_port_fail \
        "$module is not in the graphics sysroot; build its port first"
done
[[ -f "$GRAPHICS_SYSROOT/usr/share/pkgconfig/wayland-protocols.pc" ]] || \
    cross_port_fail "wayland-protocols is missing; run ports/build-wayland-protocols.sh first"

rm -rf "$BUILD" "$ROOT_DIR"
mkdir -p "$BUILD/src" "$BUILD/obj" "$ROOT_DIR"

# Patch a copy, never ports/src. The patch makes cairo optional: weston asks for
# it unconditionally even though only the demo clients, the GL borders and
# xwayland link it, and all three are off here.
tar -C "$SOURCE" --exclude=.git -cf - . | tar -C "$BUILD/src" -xf -

patches=("$PATCH_DIR"/*.patch)
[[ -e "${patches[0]}" ]] || cross_port_fail "no patches found in $PATCH_DIR"
for patch in "${patches[@]}"; do
    # patch(1) rather than `git apply`: the build copy is not a git repository,
    # and git apply quietly reports success while changing nothing there.
    # --fuzz=0 makes a patch that no longer matches an error rather than a
    # guess, so drift after a weston bump is loud.
    patch -p1 -d "$BUILD/src" --fuzz=0 --forward < "$patch" ||
        cross_port_fail "failed to apply $(basename "$patch"); it has probably drifted from weston $version"
done
grep -q 'have_cairo_shared' "$BUILD/src/shared/meson.build" || \
    cross_port_fail "the cairo patch reported success but changed nothing"

cross_port_write_meson_cross "$CROSS_FILE"
cross_port_export_pkg_config

meson setup "$BUILD/obj" "$BUILD/src" \
    --cross-file "$CROSS_FILE" \
    --prefix=/usr \
    --libdir=lib \
    --buildtype=release \
    -Dbackend-headless=true \
    -Dbackend-default=headless \
    -Dbackend-drm=false \
    -Dbackend-drm-screencast-vaapi=false \
    -Dbackend-pipewire=false \
    -Dbackend-rdp=false \
    -Dbackend-vnc=false \
    -Dbackend-wayland=false \
    -Dbackend-x11=false \
    -Drenderer-gl=false \
    -Dxwayland=false \
    -Dsystemd=false \
    -Dremoting=false \
    -Dpipewire=false \
    -Dscreenshare=false \
    -Dcolor-management-lcms=false \
    -Dimage-jpeg=false \
    -Dimage-webp=false \
    -Ddemo-clients=false \
    -Dwcap-decode=false \
    -Dsimple-clients= \
    -Dtools= \
    -Dtests=false \
    -Ddoc=false

meson compile -C "$BUILD/obj" -j "$JOBS"
DESTDIR="$ROOT_DIR" meson install -C "$BUILD/obj" --no-rebuild

[[ -x "$ROOT_DIR/usr/bin/weston" ]] || cross_port_fail "the weston binary was not installed"

# The headless backend is a dlopen()ed module, so a missing one only shows up as
# a runtime "failed to load backend".
[[ -f "$ROOT_DIR/usr/lib/libweston-14/headless-backend.so" ]] || \
    cross_port_fail "the headless backend module was not installed"

interp=$("$READELF" -l "$ROOT_DIR/usr/bin/weston" | \
    sed -n 's/.*Requesting program interpreter: \([^]]*\).*/\1/p')
[[ "$interp" == "/lib/ld-musl-x86_64.so.1" ]] || \
    cross_port_fail "weston has unexpected interpreter '${interp:-missing}'"

rm -rf "$ROOT_DIR/usr/include" "$ROOT_DIR/usr/lib/pkgconfig" \
       "$ROOT_DIR/usr/share/man" "$ROOT_DIR/usr/share/doc"
find "$ROOT_DIR/usr/lib" -maxdepth 1 -name '*.a' -delete

cross_port_finalize_root "$ROOT_DIR"
"$CROSS_STRIP" --strip-all "$ROOT_DIR/usr/bin/weston" 2>/dev/null || true

size=$(du -sh "$ROOT_DIR" | cut -f1)
printf 'weston %s (headless, pixman renderer) staged at %s (%s)\n' \
    "$version" "$ROOT_DIR" "$size"
