#!/usr/bin/env bash
set -euo pipefail

# Build zstd for Tunix. An .xbps package is a zstd-compressed tar, so libzstd is
# what lets libarchive -- and through it xbps -- open one at all.
#
# zstd builds from plain makefiles rather than autotools or meson: CC and AR are
# the whole of the cross setup. The optional codecs are turned off explicitly so
# the programs makefile does not probe the *host* for zlib, lzma and lz4 and
# then link a target binary against them.
#
# Output layout:
#   $OUT/graphics-sysroot/usr/{include,lib}   headers + .pc for libarchive
#   $OUT/zstd-root/usr/lib                    libzstd for the image
#   $OUT/zstd-root/usr/bin                    the zstd tool

PORT_NAME=zstd
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=${OUT:-$ROOT/ports/out}

# shellcheck source=ports/lib/cross-port.sh
source "$ROOT/ports/lib/cross-port.sh"

ZSTD_SOURCE="$ROOT/ports/src/zstd"
BUILD="$OUT/zstd-build"
ROOT_DIR="$OUT/zstd-root"

EXPECTED_ZSTD_VERSION=1.5.7

[[ -f "$ZSTD_SOURCE/lib/zstd.h" ]] || cross_port_fail \
    "missing $ZSTD_SOURCE; run git submodule update --init ports/src/zstd"

cross_port_require_toolchain
cross_port_require_tools make "$READELF"

zstd_version=$(sed -n 's/^#define ZSTD_VERSION_\(MAJOR\|MINOR\|RELEASE\) *//p' \
    "$ZSTD_SOURCE/lib/zstd.h" | paste -sd. -)
[[ "$zstd_version" == "$EXPECTED_ZSTD_VERSION" ]] || \
    cross_port_fail "expected zstd $EXPECTED_ZSTD_VERSION, found ${zstd_version:-unknown}"

rm -rf "$BUILD" "$ROOT_DIR"
mkdir -p "$BUILD" "$ROOT_DIR"
cp -R "$ZSTD_SOURCE/." "$BUILD/"

cross_port_export_pkg_config

make -C "$BUILD/lib" -j "$JOBS" \
    CC="$CROSS_CC" AR="$CROSS_AR" \
    ZSTD_LIB_DEPRECATED=0 \
    lib-release > "$BUILD/lib.log" 2>&1 || { tail -40 "$BUILD/lib.log"; exit 1; }

make -C "$BUILD/programs" -j "$JOBS" \
    CC="$CROSS_CC" AR="$CROSS_AR" \
    HAVE_ZLIB=0 HAVE_LZMA=0 HAVE_LZ4=0 \
    zstd > "$BUILD/programs.log" 2>&1 || { tail -40 "$BUILD/programs.log"; exit 1; }

for destination in "$GRAPHICS_SYSROOT" "$ROOT_DIR"; do
    make -C "$BUILD/lib" install \
        PREFIX=/usr LIBDIR=/usr/lib DESTDIR="$destination" > /dev/null
done
install -Dm0755 "$BUILD/programs/zstd" "$ROOT_DIR/usr/bin/zstd"

[[ -f "$GRAPHICS_SYSROOT/usr/lib/pkgconfig/libzstd.pc" ]] || \
    cross_port_fail "libzstd.pc was not installed"

library=$(find "$ROOT_DIR/usr/lib" -maxdepth 1 -type f -name 'libzstd.so*' -print -quit)
[[ -n "$library" ]] || cross_port_fail "libzstd.so was not installed"
cross_port_check_library "$library" "libzstd.so.1"

rm -rf "$ROOT_DIR/usr/include" "$ROOT_DIR/usr/lib/pkgconfig" "$ROOT_DIR/usr/share"
find "$ROOT_DIR/usr/lib" -maxdepth 1 -name '*.a' -delete
find "$ROOT_DIR/usr/lib" -maxdepth 1 -type l -name '*.so' -delete

cross_port_finalize_root "$ROOT_DIR"
"$CROSS_STRIP" --strip-all "$ROOT_DIR/usr/bin/zstd" 2>/dev/null || true
cross_port_check_runtime_closure "$ROOT_DIR" "$OUT/musl-shared-root"
