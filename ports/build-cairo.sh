#!/usr/bin/env bash
set -euo pipefail

# Build the cairo stack for Tunix: zlib, libpng, freetype and cairo.
#
# Weston needs cairo. That is not obvious from its options -- the demo clients
# and the GL renderer are the visible consumers and both are off here -- but
# libweston's headless backend itself includes gl-borders.h, which includes
# cairo-util.h, which includes cairo.h. There is no build without it.
#
# The four are built together because nothing else wants them individually and
# each is only a dependency of the next: zlib and libpng feed freetype and
# cairo's PNG surface, freetype gives cairo a font backend. This mirrors
# ports/build-image-codecs-shared.sh, which does the same for the musl-gcc
# sysroot; these are the cross-toolchain builds, for the graphics sysroot.
#
# Deliberately off: fontconfig (font *selection*, which a compositor drawing its
# own window frames does not need), the X11 and quartz surfaces, glib, and the
# test suites.
#
# Output layout:
#   $OUT/graphics-sysroot/usr/{include,lib}   headers + .pc for weston
#   $OUT/cairo-root/usr/lib                   the four shared libraries

PORT_NAME=cairo
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=${OUT:-$ROOT/ports/out}

# shellcheck source=ports/lib/cross-port.sh
source "$ROOT/ports/lib/cross-port.sh"

ZLIB_SOURCE="$ROOT/ports/src/zlib"
LIBPNG_SOURCE="$ROOT/ports/src/libpng"
FREETYPE_SOURCE="$ROOT/ports/src/freetype"
CAIRO_SOURCE="$ROOT/ports/src/cairo"

BUILD="$OUT/cairo-build"
ROOT_DIR="$OUT/cairo-root"
CROSS_FILE="$OUT/tunix-meson-cross.ini"
CMAKE_FILE="$OUT/tunix-cmake-cross.cmake"

EXPECTED_CAIRO_VERSION=1.18.4

for source in "$ZLIB_SOURCE/zlib.h" "$LIBPNG_SOURCE/png.h" \
              "$FREETYPE_SOURCE/meson.build" "$CAIRO_SOURCE/meson.build"; do
    [[ -f "$source" ]] || cross_port_fail \
        "missing $source; run git submodule update --init --recursive"
done

cross_port_require_toolchain
cross_port_require_tools meson ninja cmake make pkg-config python3 "$READELF"

# cairo computes its version at configure time rather than writing a literal
# into meson.build, so ask the same script meson does.
cairo_version=$(cd "$CAIRO_SOURCE" && python3 version.py)
[[ "$cairo_version" == "$EXPECTED_CAIRO_VERSION" ]] || \
    cross_port_fail "expected cairo $EXPECTED_CAIRO_VERSION, found ${cairo_version:-unknown}"

[[ -f "$GRAPHICS_SYSROOT/usr/lib/pkgconfig/pixman-1.pc" ]] || \
    cross_port_fail "pixman is not in the graphics sysroot; run ports/build-pixman.sh first"

rm -rf "$BUILD" "$ROOT_DIR"
mkdir -p "$BUILD" "$ROOT_DIR"

cross_port_write_meson_cross "$CROSS_FILE"
cross_port_write_cmake_toolchain "$CMAKE_FILE"
cross_port_export_pkg_config

# Install into both the sysroot (so the next library in the chain can find it)
# and the staged root (so it reaches the image).
install_both() {
    local build_dir="$1" kind="$2"
    case "$kind" in
        meson)
            DESTDIR="$GRAPHICS_SYSROOT" meson install -C "$build_dir" --no-rebuild
            DESTDIR="$ROOT_DIR" meson install -C "$build_dir" --no-rebuild
            ;;
        cmake)
            DESTDIR="$GRAPHICS_SYSROOT" cmake --install "$build_dir"
            DESTDIR="$ROOT_DIR" cmake --install "$build_dir"
            ;;
    esac
}

# --- zlib ----------------------------------------------------------------
# Its configure is hand-written, not autotools, and takes the compiler through
# the environment rather than --host.
mkdir -p "$BUILD/zlib"
(
    cd "$BUILD/zlib"
    env CC="$CROSS_CC" AR="$CROSS_AR" CFLAGS="-O2 -fPIC" \
        bash "$ZLIB_SOURCE/configure" --prefix=/usr --libdir=/usr/lib
    make -j"$JOBS"
    make install DESTDIR="$GRAPHICS_SYSROOT"
    make install DESTDIR="$ROOT_DIR"
)
[[ -f "$GRAPHICS_SYSROOT/usr/include/zlib.h" ]] || cross_port_fail "zlib headers were not installed"

# --- libpng --------------------------------------------------------------
cmake -S "$LIBPNG_SOURCE" -B "$BUILD/libpng" -G "Unix Makefiles" \
    -DCMAKE_TOOLCHAIN_FILE="$CMAKE_FILE" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/usr \
    -DCMAKE_INSTALL_LIBDIR=lib \
    -DCMAKE_C_FLAGS="-O2 -fPIC" \
    -DPNG_SHARED=ON -DPNG_STATIC=OFF \
    -DPNG_TESTS=OFF -DPNG_TOOLS=OFF -DPNG_EXECUTABLES=OFF \
    -DPNG_HARDWARE_OPTIMIZATIONS=OFF \
    -DZLIB_ROOT="$GRAPHICS_SYSROOT/usr" \
    -DZLIB_INCLUDE_DIR="$GRAPHICS_SYSROOT/usr/include" \
    -DZLIB_LIBRARY="$GRAPHICS_SYSROOT/usr/lib/libz.so"
cmake --build "$BUILD/libpng" --parallel "$JOBS"
install_both "$BUILD/libpng" cmake
[[ -f "$GRAPHICS_SYSROOT/usr/include/png.h" ]] || cross_port_fail "libpng headers were not installed"

# --- freetype ------------------------------------------------------------
# harfbuzz and brotli are for advanced shaping and woff2, neither of which a
# compositor drawing frame decorations needs.
meson setup "$BUILD/freetype" "$FREETYPE_SOURCE" \
    --cross-file "$CROSS_FILE" \
    --prefix=/usr --libdir=lib --buildtype=release --default-library=shared \
    -Dharfbuzz=disabled -Dbrotli=disabled -Dbzip2=disabled -Dtests=disabled
meson compile -C "$BUILD/freetype" -j "$JOBS"
install_both "$BUILD/freetype" meson
[[ -f "$GRAPHICS_SYSROOT/usr/lib/pkgconfig/freetype2.pc" ]] || \
    cross_port_fail "freetype2.pc was not installed"

# --- cairo ---------------------------------------------------------------
meson setup "$BUILD/cairo" "$CAIRO_SOURCE" \
    --cross-file "$CROSS_FILE" \
    --prefix=/usr --libdir=lib --buildtype=release --default-library=shared \
    -Dfreetype=enabled \
    -Dpng=enabled \
    -Dzlib=enabled \
    -Dfontconfig=disabled \
    -Dxlib=disabled \
    -Dxcb=disabled \
    -Dquartz=disabled \
    -Dtee=disabled \
    -Dglib=disabled \
    -Dspectre=disabled \
    -Dsymbol-lookup=disabled \
    -Dtests=disabled \
    -Dgtk2-utils=disabled
meson compile -C "$BUILD/cairo" -j "$JOBS"
install_both "$BUILD/cairo" meson

[[ -f "$GRAPHICS_SYSROOT/usr/include/cairo/cairo.h" ]] || \
    cross_port_fail "cairo headers were not installed into the graphics sysroot"
[[ -f "$GRAPHICS_SYSROOT/usr/lib/pkgconfig/cairo.pc" ]] || \
    cross_port_fail "cairo.pc was not installed into the graphics sysroot"

for spec in "libz.so.1:libz.so.1" "libpng16.so.16:libpng16.so.16" \
            "libfreetype.so.6:libfreetype.so.6" "libcairo.so.2:libcairo.so.2"; do
    name=${spec%%:*}
    soname=${spec##*:}
    library=$(find "$ROOT_DIR/usr/lib" -maxdepth 1 -type f -name "$name*" -print -quit)
    [[ -n "$library" ]] || cross_port_fail "$name was not installed"
    cross_port_check_library "$library" "$soname"
done

rm -rf "$ROOT_DIR/usr/include" "$ROOT_DIR/usr/lib/pkgconfig" "$ROOT_DIR/usr/share" \
       "$ROOT_DIR/usr/bin" "$ROOT_DIR/usr/lib/cmake"
find "$ROOT_DIR/usr/lib" -maxdepth 1 -name '*.a' -delete
find "$ROOT_DIR/usr/lib" -maxdepth 1 -type l -name '*.so' -delete

cross_port_finalize_root "$ROOT_DIR"
cross_port_check_runtime_closure "$ROOT_DIR" "$OUT/pixman-root"

size=$(du -sh "$ROOT_DIR" | cut -f1)
printf 'cairo stack (zlib, libpng, freetype %s) staged at %s (%s)\n' \
    "$cairo_version" "$ROOT_DIR" "$size"
