#!/usr/bin/env bash
set -euo pipefail

# Build libwayland for Tunix.
#
# This is the protocol library every Wayland compositor and client is written
# against, and the first port that actually exercises the kernel work done for
# it: the wire protocol is a unix socket, buffers travel as descriptors over
# SCM_RIGHTS, and clients allocate them with memfd_create.
#
# Two things are deliberately not built:
#
#   wayland-scanner  It is a build-time code generator, not something Tunix
#                    needs to run, and building it for the target would drag in
#                    expat. Meson uses the *host* scanner in a cross build
#                    anyway, which is why the version has to match exactly.
#   tests/docs       Neither ships in the image.
#
# Output layout:
#   $OUT/graphics-sysroot/usr/{include,lib}   headers + .pc for weston later
#   $OUT/wayland-root/usr/lib                 libwayland-{server,client,cursor}
#   $OUT/wayland-root/usr/share/wayland       the protocol XML

PORT_NAME=wayland
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=${OUT:-$ROOT/ports/out}

# shellcheck source=ports/lib/cross-port.sh
source "$ROOT/ports/lib/cross-port.sh"

SOURCE="$ROOT/ports/src/wayland"
BUILD="$OUT/wayland-build"
ROOT_DIR="$OUT/wayland-root"
CROSS_FILE="$OUT/tunix-meson-cross.ini"
TEST_SOURCE="$ROOT/tools/wayland-roundtrip-test.c"

EXPECTED_VERSION=1.25.0

[[ -f "$SOURCE/meson.build" ]] || cross_port_fail \
    "missing wayland source at $SOURCE; run git submodule update --init --recursive"
[[ -f "$TEST_SOURCE" ]] || cross_port_fail "missing the roundtrip test at $TEST_SOURCE"

cross_port_require_toolchain
cross_port_require_tools meson ninja pkg-config "$READELF"

version=$(sed -n "s/^[[:space:]]*version:[[:space:]]*'\([0-9.]*\)'.*/\1/p" \
    "$SOURCE/meson.build" | head -n1)
[[ "$version" == "$EXPECTED_VERSION" ]] || \
    cross_port_fail "expected wayland $EXPECTED_VERSION, found ${version:-unknown}"

# In a cross build meson resolves wayland-scanner from the *native* pkg-config
# and requires the exact project version, because the generated glue has to
# match the library it is compiled into.
if ! pkg-config --exists wayland-scanner; then
    cross_port_fail "the host needs wayland-scanner $EXPECTED_VERSION (e.g. pacman -S wayland)"
fi
host_scanner=$(pkg-config --modversion wayland-scanner)
[[ "$host_scanner" == "$EXPECTED_VERSION" ]] || cross_port_fail \
    "the host wayland-scanner is $host_scanner but the port builds $EXPECTED_VERSION; they must match"

[[ -f "$GRAPHICS_SYSROOT/usr/lib/pkgconfig/libffi.pc" ]] || cross_port_fail \
    "libffi is not in the graphics sysroot; run ports/build-libffi.sh first"
LIBFFI_ROOT="$OUT/libffi-root"
[[ -d "$LIBFFI_ROOT/usr/lib" ]] || cross_port_fail \
    "the staged libffi root is missing; run ports/build-libffi.sh first"

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
    -Dlibraries=true \
    -Dscanner=false \
    -Dtests=false \
    -Ddocumentation=false \
    -Ddtd_validation=false

meson compile -C "$BUILD" -j "$JOBS"

DESTDIR="$GRAPHICS_SYSROOT" meson install -C "$BUILD" --no-rebuild
DESTDIR="$ROOT_DIR" meson install -C "$BUILD" --no-rebuild

for header in wayland-server-core.h wayland-client-core.h wayland-server-protocol.h; do
    [[ -f "$GRAPHICS_SYSROOT/usr/include/$header" ]] || \
        cross_port_fail "$header was not installed into the graphics sysroot"
done
for module in wayland-server wayland-client; do
    [[ -f "$GRAPHICS_SYSROOT/usr/lib/pkgconfig/$module.pc" ]] || \
        cross_port_fail "$module.pc was not installed into the graphics sysroot"
done

check_shared() {
    local name="$1" soname="$2"
    local library
    library=$(find "$ROOT_DIR/usr/lib" -maxdepth 1 -type f -name "$name" -print -quit)
    [[ -n "$library" ]] || cross_port_fail "$name was not installed"
    cross_port_check_library "$library" "$soname"
}

check_shared 'libwayland-server.so.0.*' libwayland-server.so.0
check_shared 'libwayland-client.so.0.*' libwayland-client.so.0
check_shared 'libwayland-cursor.so.0.*' libwayland-cursor.so.0

# No protocol XML is installed, and that is correct: upstream ships it only
# alongside wayland-scanner, which we build on the host. Clients and compositors
# link against the C glue the scanner already generated -- nothing reads the XML
# at runtime.

rm -rf "$ROOT_DIR/usr/include" "$ROOT_DIR/usr/lib/pkgconfig" \
       "$ROOT_DIR/usr/share"
find "$ROOT_DIR/usr/lib" -maxdepth 1 -name '*.a' -delete
# Drop the unversioned linker symlinks; the image resolves everything by SONAME.
find "$ROOT_DIR/usr/lib" -maxdepth 1 -type l -name '*.so' -delete

# Build the roundtrip test against what we just installed. It is the only proof
# that the protocol actually works end to end rather than merely linking.
mkdir -p "$ROOT_DIR/usr/bin"
"$CROSS_CC" -std=c11 -Wall -Wextra -O2 -fPIE -pie \
    -I"$GRAPHICS_SYSROOT/usr/include" \
    "$TEST_SOURCE" \
    -L"$GRAPHICS_SYSROOT/usr/lib" \
    -Wl,-rpath-link,"$GRAPHICS_SYSROOT/usr/lib" \
    -lwayland-server -lwayland-client \
    -o "$ROOT_DIR/usr/bin/wayland-roundtrip-test"

needed=$("$READELF" -d "$ROOT_DIR/usr/bin/wayland-roundtrip-test" | \
    sed -n 's/.*Shared library: \[\([^]]*\)\].*/\1/p')
for dependency in libwayland-server.so.0 libwayland-client.so.0; do
    grep -Fxq "$dependency" <<<"$needed" || \
        cross_port_fail "the roundtrip test is missing NEEDED $dependency"
done

"$CROSS_STRIP" --strip-all "$ROOT_DIR/usr/bin/wayland-roundtrip-test"

cross_port_finalize_root "$ROOT_DIR"
cross_port_check_runtime_closure "$ROOT_DIR" "$LIBFFI_ROOT"

printf 'wayland %s staged at %s\n' "$version" "$ROOT_DIR"
