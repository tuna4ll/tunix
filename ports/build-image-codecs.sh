#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)}
OUT=${OUT:-$ROOT/ports/out}
SYSROOT="$OUT/sysroot"
MUSL_CC="$SYSROOT/usr/bin/musl-gcc"
ZLIB_SOURCE="$ROOT/ports/src/zlib"
LIBPNG_SOURCE="$ROOT/ports/src/libpng"
LIBJPEG_SOURCE="$ROOT/ports/src/libjpeg-turbo"
ZLIB_BUILD="$OUT/zlib-build"
LIBPNG_BUILD="$OUT/libpng-build"
LIBJPEG_BUILD="$OUT/libjpeg-turbo-build"
CODECS_ROOT="$OUT/image-codecs-root"
WALLPAPER_TOOL="$OUT/tunix-wallpaper"
HOST_AR=${AR:-ar}
HOST_RANLIB=${RANLIB:-ranlib}

EXPECTED_ZLIB_VERSION=1.3.2
EXPECTED_LIBPNG_VERSION=1.6.58
EXPECTED_LIBJPEG_VERSION=3.2.0

fail() {
    echo "build-image-codecs: $*" >&2
    exit 1
}

require_file() {
    [[ -f "$1" ]] || fail "$2"
}

require_tool() {
    command -v "$1" >/dev/null 2>&1 || fail "$1 was not found"
}

require_file "$ZLIB_SOURCE/zlib.h" "missing zlib source; initialize image codec submodules"
require_file "$LIBPNG_SOURCE/png.h" "missing libpng source; initialize image codec submodules"
require_file "$LIBJPEG_SOURCE/CMakeLists.txt" "missing libjpeg-turbo source; initialize image codec submodules"
require_file "$ROOT/tools/tunix-wallpaper.c" "missing Tunix wallpaper converter source"
[[ -x "$ZLIB_SOURCE/configure" ]] || fail "zlib configure script is missing"
[[ -x "$MUSL_CC" ]] || fail "missing Tunix musl toolchain; build Bash first"

for tool in cmake make "$HOST_AR" "$HOST_RANLIB"; do
    require_tool "$tool"
done

zlib_version=$(sed -n 's/^#define ZLIB_VERSION[[:space:]]*"\([^"]*\)".*/\1/p' "$ZLIB_SOURCE/zlib.h" | head -n1)
libpng_version=$(sed -n 's/^#define PNG_LIBPNG_VER_STRING[[:space:]]*"\([^"]*\)".*/\1/p' "$LIBPNG_SOURCE/png.h" | head -n1)
libjpeg_version=$(sed -n 's/^set(VERSION[[:space:]]*\([^)]*\)).*/\1/p' "$LIBJPEG_SOURCE/CMakeLists.txt" | head -n1)
[[ "$zlib_version" == "$EXPECTED_ZLIB_VERSION" ]] || \
    fail "expected zlib $EXPECTED_ZLIB_VERSION, found ${zlib_version:-unknown}"
[[ "$libpng_version" == "$EXPECTED_LIBPNG_VERSION" ]] || \
    fail "expected libpng $EXPECTED_LIBPNG_VERSION, found ${libpng_version:-unknown}"
[[ "$libjpeg_version" == "$EXPECTED_LIBJPEG_VERSION" ]] || \
    fail "expected libjpeg-turbo $EXPECTED_LIBJPEG_VERSION, found ${libjpeg_version:-unknown}"

NO_AUTO_ATOMIC=""
probe=$(mktemp)
probe_object="$probe.o"
probe_wallpaper="$OUT/.wallpaper-codec-probe.twl"
trap 'rm -f "$probe" "$probe_object" "$probe_wallpaper"' EXIT
printf 'int tunix_probe;\n' > "$probe"
if "$MUSL_CC" -fno-link-libatomic -x c -c "$probe" -o "$probe_object" >/dev/null 2>&1; then
    NO_AUTO_ATOMIC="-fno-link-libatomic"
fi

COMMON_CFLAGS="-Os -fno-stack-protector -fno-pie $NO_AUTO_ATOMIC"
COMMON_LDFLAGS="-static -no-pie $NO_AUTO_ATOMIC"

rm -rf "$ZLIB_BUILD" "$LIBPNG_BUILD" "$LIBJPEG_BUILD" "$CODECS_ROOT"
mkdir -p "$ZLIB_BUILD" "$LIBPNG_BUILD" "$LIBJPEG_BUILD" "$CODECS_ROOT"

(
    cd "$ZLIB_BUILD"
    env CC="$MUSL_CC" AR="$HOST_AR" RANLIB="$HOST_RANLIB" \
        CFLAGS="$COMMON_CFLAGS" LDFLAGS="$COMMON_LDFLAGS" \
        "$ZLIB_SOURCE/configure" --prefix=/usr --static
    make -j"$JOBS"
    make install DESTDIR="$CODECS_ROOT"
)

require_file "$CODECS_ROOT/usr/include/zlib.h" "zlib headers were not installed"
require_file "$CODECS_ROOT/usr/lib/libz.a" "zlib static library was not installed"

cmake -S "$LIBPNG_SOURCE" -B "$LIBPNG_BUILD" -G "Unix Makefiles" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER="$MUSL_CC" \
    -DCMAKE_AR="$HOST_AR" \
    -DCMAKE_RANLIB="$HOST_RANLIB" \
    -DCMAKE_INSTALL_PREFIX=/usr \
    -DCMAKE_INSTALL_LIBDIR=lib \
    -DCMAKE_C_FLAGS="$COMMON_CFLAGS" \
    -DCMAKE_EXE_LINKER_FLAGS="$COMMON_LDFLAGS" \
    -DCMAKE_POSITION_INDEPENDENT_CODE=OFF \
    -DPNG_SHARED=OFF \
    -DPNG_STATIC=ON \
    -DPNG_TESTS=OFF \
    -DPNG_TOOLS=OFF \
    -DPNG_EXECUTABLES=OFF \
    -DPNG_HARDWARE_OPTIMIZATIONS=OFF \
    -DZLIB_ROOT="$CODECS_ROOT/usr" \
    -DZLIB_INCLUDE_DIR="$CODECS_ROOT/usr/include" \
    -DZLIB_LIBRARY="$CODECS_ROOT/usr/lib/libz.a"
cmake --build "$LIBPNG_BUILD" --parallel "$JOBS"
DESTDIR="$CODECS_ROOT" cmake --install "$LIBPNG_BUILD"

PNG_LIBRARY="$CODECS_ROOT/usr/lib/libpng16.a"
[[ -f "$PNG_LIBRARY" ]] || PNG_LIBRARY="$CODECS_ROOT/usr/lib/libpng.a"
require_file "$CODECS_ROOT/usr/include/png.h" "libpng headers were not installed"
require_file "$PNG_LIBRARY" "libpng static library was not installed"

cmake -S "$LIBJPEG_SOURCE" -B "$LIBJPEG_BUILD" -G "Unix Makefiles" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER="$MUSL_CC" \
    -DCMAKE_AR="$HOST_AR" \
    -DCMAKE_RANLIB="$HOST_RANLIB" \
    -DCMAKE_INSTALL_PREFIX=/usr \
    -DCMAKE_INSTALL_LIBDIR=lib \
    -DCMAKE_C_FLAGS="$COMMON_CFLAGS" \
    -DCMAKE_EXE_LINKER_FLAGS="$COMMON_LDFLAGS" \
    -DCMAKE_POSITION_INDEPENDENT_CODE=OFF \
    -DENABLE_SHARED=OFF \
    -DENABLE_STATIC=ON \
    -DREQUIRE_SIMD=OFF \
    -DWITH_SIMD=OFF \
    -DWITH_TURBOJPEG=ON \
    -DWITH_TOOLS=OFF \
    -DWITH_TESTS=OFF
cmake --build "$LIBJPEG_BUILD" --parallel "$JOBS"
DESTDIR="$CODECS_ROOT" cmake --install "$LIBJPEG_BUILD"

require_file "$CODECS_ROOT/usr/include/jpeglib.h" "libjpeg-turbo headers were not installed"
require_file "$CODECS_ROOT/usr/include/turbojpeg.h" "TurboJPEG headers were not installed"
require_file "$CODECS_ROOT/usr/lib/libjpeg.a" "libjpeg static library was not installed"
require_file "$CODECS_ROOT/usr/lib/libturbojpeg.a" "TurboJPEG static library was not installed"

"$MUSL_CC" -std=c11 -Wall -Wextra -Werror $COMMON_CFLAGS \
    -I"$CODECS_ROOT/usr/include" \
    -DTUNIX_LIBJPEG_TURBO_VERSION=\"$libjpeg_version\" \
    -DTUNIX_ZLIB_VERSION=\"$zlib_version\" \
    "$ROOT/tools/tunix-wallpaper.c" \
    "$PNG_LIBRARY" \
    "$CODECS_ROOT/usr/lib/libjpeg.a" \
    "$CODECS_ROOT/usr/lib/libz.a" \
    -lm $COMMON_LDFLAGS -o "$WALLPAPER_TOOL"
chmod 0755 "$WALLPAPER_TOOL"

"$WALLPAPER_TOOL" --self-test
"$WALLPAPER_TOOL" "$ROOT/assets/tunix-mountain-lake.jpg" "$probe_wallpaper" --width 64 --height 36
probe_size=$(wc -c < "$probe_wallpaper")
[[ "$probe_size" -eq 4632 ]] || fail "wallpaper probe has unexpected size $probe_size"

if command -v readelf >/dev/null 2>&1; then
    if readelf -l "$WALLPAPER_TOOL" | grep -q 'INTERP'; then
        fail "wallpaper converter unexpectedly contains a dynamic interpreter"
    fi
    if readelf -d "$WALLPAPER_TOOL" 2>/dev/null | grep -q 'NEEDED'; then
        fail "wallpaper converter unexpectedly contains dynamic dependencies"
    fi
fi

mkdir -p "$CODECS_ROOT/usr/share/licenses/zlib" \
         "$CODECS_ROOT/usr/share/licenses/libpng" \
         "$CODECS_ROOT/usr/share/licenses/libjpeg-turbo"
[[ -f "$ZLIB_SOURCE/LICENSE" ]] && cp "$ZLIB_SOURCE/LICENSE" "$CODECS_ROOT/usr/share/licenses/zlib/"
[[ -f "$LIBPNG_SOURCE/LICENSE.md" ]] && cp "$LIBPNG_SOURCE/LICENSE.md" "$CODECS_ROOT/usr/share/licenses/libpng/"
[[ -f "$LIBJPEG_SOURCE/LICENSE.md" ]] && cp "$LIBJPEG_SOURCE/LICENSE.md" "$CODECS_ROOT/usr/share/licenses/libjpeg-turbo/"

printf '%s\n' \
    "zlib=$zlib_version" \
    "libpng=$libpng_version" \
    "libjpeg-turbo=$libjpeg_version" \
    > "$CODECS_ROOT/usr/share/tunix-image-codecs.version"

printf 'Image codecs ready: zlib %s, libpng %s, libjpeg-turbo %s\n' \
    "$zlib_version" "$libpng_version" "$libjpeg_version"
