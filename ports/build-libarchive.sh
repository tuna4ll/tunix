#!/usr/bin/env bash
set -euo pipefail

# Build libarchive for Tunix. It is what opens an .xbps package: a tar stream
# inside zstd, which is exactly the pair this is configured for.
#
# The format list is cut to what xbps actually reads. Everything else -- the
# other archive formats, the other compressors, xattrs, ACLs, iconv, xml -- is
# turned off rather than left to autodetect, because autodetect here means
# "look at the build host", and the build host has libraries Tunix does not.
#
# ACL and xattr support in particular must go: Tunix has no xattr syscalls at
# all, and a libarchive that thinks it can set them would fail every extraction
# rather than skipping the attribute.
#
# Output layout:
#   $OUT/graphics-sysroot/usr/{include,lib}   headers + .pc for xbps
#   $OUT/libarchive-root/usr/lib              libarchive for the image
#   $OUT/libarchive-root/usr/bin              bsdtar/bsdcpio

PORT_NAME=libarchive
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=${OUT:-$ROOT/ports/out}

# shellcheck source=ports/lib/cross-port.sh
source "$ROOT/ports/lib/cross-port.sh"

LIBARCHIVE_SOURCE="$ROOT/ports/src/libarchive"
BUILD="$OUT/libarchive-build"
ROOT_DIR="$OUT/libarchive-root"

EXPECTED_LIBARCHIVE_VERSION=3.8.9

[[ -f "$LIBARCHIVE_SOURCE/configure.ac" ]] || cross_port_fail \
    "missing $LIBARCHIVE_SOURCE; run git submodule update --init ports/src/libarchive"

cross_port_require_toolchain
cross_port_require_tools make autoreconf libtoolize pkg-config "$READELF"

libarchive_version=$(sed -n 's/^m4_define(\[LIBARCHIVE_VERSION_S\],\[\([^]]*\)\]).*/\1/p' \
    "$LIBARCHIVE_SOURCE/configure.ac")
[[ "$libarchive_version" == "$EXPECTED_LIBARCHIVE_VERSION" ]] || \
    cross_port_fail "expected libarchive $EXPECTED_LIBARCHIVE_VERSION, found ${libarchive_version:-unknown}"

for module in libzstd libcrypto zlib; do
    [[ -f "$GRAPHICS_SYSROOT/usr/lib/pkgconfig/$module.pc" ]] || cross_port_fail \
        "$module is not in the graphics sysroot; build its port first"
done

rm -rf "$BUILD" "$ROOT_DIR"
mkdir -p "$BUILD" "$ROOT_DIR"

# The git tree ships no configure; generate one in a copy so the submodule
# checkout stays clean.
SOURCE="$OUT/libarchive-src"
rm -rf "$SOURCE"
cp -R "$LIBARCHIVE_SOURCE" "$SOURCE"
( cd "$SOURCE" && autoreconf -fi > autoreconf.log 2>&1 ) || \
    { tail -30 "$SOURCE/autoreconf.log"; cross_port_fail "autoreconf failed"; }

cross_port_export_pkg_config
cross_port_autotools_setup

cross_port_configure "$SOURCE" "$BUILD" \
    --without-bz2lib --without-libb2 --without-iconv --without-lz4 \
    --without-lzma --without-lzo2 --without-nettle --without-xml2 \
    --without-expat --without-cng \
    --with-zlib --with-zstd --with-openssl \
    --disable-acl --disable-xattr \
    --disable-bsdcat --disable-bsdunzip \
    > "$BUILD/configure.log" 2>&1 || { tail -40 "$BUILD/configure.log"; exit 1; }

make -C "$BUILD" -j "$JOBS" > "$BUILD/build.log" 2>&1 || { tail -60 "$BUILD/build.log"; exit 1; }

make -C "$BUILD" install DESTDIR="$GRAPHICS_SYSROOT" > /dev/null
make -C "$BUILD" install DESTDIR="$ROOT_DIR" > /dev/null

[[ -f "$GRAPHICS_SYSROOT/usr/lib/pkgconfig/libarchive.pc" ]] || \
    cross_port_fail "libarchive.pc was not installed"
cross_port_check_library "$ROOT_DIR/usr/lib/libarchive.so.13" "libarchive.so.13"

rm -rf "$ROOT_DIR/usr/include" "$ROOT_DIR/usr/lib/pkgconfig" "$ROOT_DIR/usr/share"
find "$ROOT_DIR/usr/lib" -maxdepth 1 -name '*.a' -delete
find "$ROOT_DIR/usr/lib" -maxdepth 1 -name '*.la' -delete
find "$ROOT_DIR/usr/lib" -maxdepth 1 -type l -name '*.so' -delete

cross_port_finalize_root "$ROOT_DIR"
find "$ROOT_DIR/usr/bin" -type f -exec "$CROSS_STRIP" --strip-all {} + 2>/dev/null || true
cross_port_check_runtime_closure "$ROOT_DIR" "$OUT/zstd-root" "$OUT/openssl-root" \
    "$OUT/musl-shared-root" "$OUT/image-codecs-shared-root"
