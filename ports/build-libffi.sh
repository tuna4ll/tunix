#!/usr/bin/env bash
set -euo pipefail

# Build libffi for Tunix.
#
# libwayland calls into message handlers whose signatures are only known at
# runtime, from the protocol description, and libffi is what lets it assemble
# those calls. It is a hard dependency of libwayland-server, so it is the first
# link in the chain towards Weston.
#
# Built with the musl cross toolchain rather than musl-gcc: it lands in the same
# graphics sysroot as libdrm and mesa, and everything in that sysroot has to
# agree on one libc.
#
# Output layout:
#   $OUT/graphics-sysroot/usr/{include,lib}   headers + .pc for later ports
#   $OUT/libffi-root/usr/lib                  shared library staged for the image

PORT_NAME=libffi
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=${OUT:-$ROOT/ports/out}

# shellcheck source=ports/lib/cross-port.sh
source "$ROOT/ports/lib/cross-port.sh"

SOURCE="$ROOT/ports/src/libffi"
BUILD="$OUT/libffi-build"
ROOT_DIR="$OUT/libffi-root"

EXPECTED_VERSION=3.7.1

[[ -f "$SOURCE/configure.ac" ]] || cross_port_fail \
    "missing libffi source at $SOURCE; run git submodule update --init --recursive"

cross_port_require_toolchain
cross_port_require_tools autoreconf automake autoconf libtool make "$READELF"

version=$(sed -n 's/^AC_INIT(\[libffi\], *\[\([0-9.]*\)\].*/\1/p' "$SOURCE/configure.ac" | head -n1)
[[ "$version" == "$EXPECTED_VERSION" ]] || \
    cross_port_fail "expected libffi $EXPECTED_VERSION, found ${version:-unknown}"

rm -rf "$BUILD" "$ROOT_DIR"
mkdir -p "$BUILD/src" "$BUILD/obj" "$ROOT_DIR"

# A git checkout ships no configure script, and autotools can only generate one
# inside the tree it describes. Work on a copy so ports/src stays pristine.
tar -C "$SOURCE" --exclude=.git -cf - . | tar -C "$BUILD/src" -xf -

# libffi declares its macro directory twice, the modern way in configure.ac and
# the historical way in Makefile.am. libtool 2.5 and newer treat that as an
# error rather than a redundancy, so drop the historical one from our copy;
# AC_CONFIG_MACRO_DIR([m4]) already says the same thing.
sed -i '/^ACLOCAL_AMFLAGS[[:space:]]*=/d' "$BUILD/src/Makefile.am"

(cd "$BUILD/src" && autoreconf -fi) || cross_port_fail "autoreconf failed"
[[ -x "$BUILD/src/configure" ]] || cross_port_fail "configure was not generated"

(
    cd "$BUILD/obj"
    # --host without --build is what tells autotools this is a cross build; the
    # toolchain triplet matches the compiler prefix so libtool finds the rest.
    "$BUILD/src/configure" \
        --host="$CROSS_TARGET" \
        --prefix=/usr \
        --libdir=/usr/lib \
        --disable-static \
        --enable-shared \
        --disable-multi-os-directory \
        --disable-docs \
        CC="$CROSS_CC" \
        CXX="$CROSS_CXX" \
        AR="$CROSS_AR" \
        STRIP="$CROSS_STRIP" \
        CFLAGS="-O2 -fPIC"

    # configure sources configure.host to pick the architecture backend, and a
    # failure to source it is not fatal -- TARGETDIR simply stays "unknown" and
    # the build dies much later copying src/unknown/ffitarget.h. The way that
    # happens in practice is CRLF line endings: libffi's .gitattributes marks
    # everything text=auto, so a checkout on Windows rewrites the shell scripts
    # and the parse fails. Catch it here, where the cause is still obvious.
    if grep -q 'TARGETDIR="unknown"' config.status; then
        cross_port_fail "configure did not detect the target architecture; if ports/src/libffi has CRLF line endings, re-check it out with core.eol=lf"
    fi

    make -j"$JOBS"
    make install DESTDIR="$GRAPHICS_SYSROOT"
    make install DESTDIR="$ROOT_DIR"
)

[[ -f "$GRAPHICS_SYSROOT/usr/include/ffi.h" ]] || \
    cross_port_fail "libffi headers were not installed into the graphics sysroot"
[[ -f "$GRAPHICS_SYSROOT/usr/lib/pkgconfig/libffi.pc" ]] || \
    cross_port_fail "libffi.pc was not installed into the graphics sysroot"

shared=$(find "$ROOT_DIR/usr/lib" -maxdepth 1 -type f -name 'libffi.so.8*' -print -quit)
[[ -n "$shared" ]] || cross_port_fail "libffi shared library was not installed"
cross_port_check_library "$shared" libffi.so.8

# Development-only files stay in the sysroot.
rm -rf "$ROOT_DIR/usr/include" "$ROOT_DIR/usr/lib/pkgconfig" "$ROOT_DIR/usr/share"
find "$ROOT_DIR/usr/lib" -maxdepth 1 \( -name '*.a' -o -name '*.la' \) -delete
rm -f "$ROOT_DIR/usr/lib/libffi.so"

cross_port_finalize_root "$ROOT_DIR"

printf 'libffi %s staged at %s\n' "$version" "$ROOT_DIR"
