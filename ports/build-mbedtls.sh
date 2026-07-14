#!/usr/bin/env bash
set -euo pipefail

# Build mbedTLS as static musl libraries for Tunix and link the https-get
# client tool against them. Mirrors ports/build-image-codecs.sh: everything is
# compiled with the shared musl toolchain and staged under ports/out.

ROOT=$(cd "$(dirname "$0")/.." && pwd)
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)}
OUT=${OUT:-$ROOT/ports/out}
SYSROOT="$OUT/sysroot"
MUSL_CC="$SYSROOT/usr/bin/musl-gcc"
MBEDTLS_SOURCE="$ROOT/ports/src/mbedtls"
MBEDTLS_BUILD="$OUT/mbedtls-build"
MBEDTLS_ROOT="$OUT/mbedtls-root"
HTTPS_GET_SOURCE="$ROOT/tools/https-get.c"
HTTPS_GET_TOOL="$OUT/https-get"
SSL_HELPER_SOURCE="$ROOT/tools/ssl-helper.c"
SSL_HELPER_TOOL="$OUT/openssl"
HOST_AR=${AR:-ar}
HOST_RANLIB=${RANLIB:-ranlib}

# Accept any release from the mbedTLS 3.6 LTS series (3.6.x). The whole port was
# written against this API; the 4.x major release is a breaking change (crypto
# split into a separate repo, different CMake/API) and is intentionally rejected.
EXPECTED_MBEDTLS_SERIES="3.6"

fail() {
    echo "build-mbedtls: $*" >&2
    exit 1
}

require_file() {
    [[ -f "$1" ]] || fail "$2"
}

require_file "$MBEDTLS_SOURCE/CMakeLists.txt" "missing mbedTLS source; initialize the mbedtls submodule"
require_file "$MBEDTLS_SOURCE/include/mbedtls/build_info.h" "missing mbedTLS headers"
require_file "$HTTPS_GET_SOURCE" "missing tools/https-get.c"
require_file "$SSL_HELPER_SOURCE" "missing tools/ssl-helper.c"
[[ -x "$MUSL_CC" ]] || fail "missing Tunix musl toolchain; build Bash first"

for tool in cmake make "$HOST_AR" "$HOST_RANLIB"; do
    command -v "$tool" >/dev/null 2>&1 || fail "$tool was not found"
done

version_major=$(sed -n 's/^#define MBEDTLS_VERSION_MAJOR[[:space:]]*\([0-9]*\).*/\1/p' "$MBEDTLS_SOURCE/include/mbedtls/build_info.h")
version_minor=$(sed -n 's/^#define MBEDTLS_VERSION_MINOR[[:space:]]*\([0-9]*\).*/\1/p' "$MBEDTLS_SOURCE/include/mbedtls/build_info.h")
version_patch=$(sed -n 's/^#define MBEDTLS_VERSION_PATCH[[:space:]]*\([0-9]*\).*/\1/p' "$MBEDTLS_SOURCE/include/mbedtls/build_info.h")
mbedtls_version="${version_major}.${version_minor}.${version_patch}"
[[ "${version_major}.${version_minor}" == "$EXPECTED_MBEDTLS_SERIES" ]] || \
    fail "expected mbedTLS $EXPECTED_MBEDTLS_SERIES.x (LTS), found ${mbedtls_version:-unknown}"

NO_AUTO_ATOMIC=""
probe=$(mktemp)
probe_object="$probe.o"
trap 'rm -f "$probe" "$probe_object"' EXIT
printf 'int tunix_probe;\n' > "$probe"
if "$MUSL_CC" -fno-link-libatomic -x c -c "$probe" -o "$probe_object" >/dev/null 2>&1; then
    NO_AUTO_ATOMIC="-fno-link-libatomic"
fi

COMMON_CFLAGS="-Os -fno-stack-protector -fno-pie $NO_AUTO_ATOMIC"
COMMON_LDFLAGS="-static -no-pie $NO_AUTO_ATOMIC"

rm -rf "$MBEDTLS_BUILD" "$MBEDTLS_ROOT"
mkdir -p "$MBEDTLS_BUILD" "$MBEDTLS_ROOT"

cmake -S "$MBEDTLS_SOURCE" -B "$MBEDTLS_BUILD" -G "Unix Makefiles" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER="$MUSL_CC" \
    -DCMAKE_AR="$(command -v "$HOST_AR")" \
    -DCMAKE_RANLIB="$(command -v "$HOST_RANLIB")" \
    -DCMAKE_INSTALL_PREFIX=/usr \
    -DCMAKE_INSTALL_LIBDIR=lib \
    -DCMAKE_C_FLAGS="$COMMON_CFLAGS" \
    -DCMAKE_EXE_LINKER_FLAGS="$COMMON_LDFLAGS" \
    -DCMAKE_POSITION_INDEPENDENT_CODE=OFF \
    -DENABLE_TESTING=OFF \
    -DENABLE_PROGRAMS=OFF \
    -DMBEDTLS_FATAL_WARNINGS=OFF \
    -DUSE_SHARED_MBEDTLS_LIBRARY=OFF \
    -DUSE_STATIC_MBEDTLS_LIBRARY=ON
cmake --build "$MBEDTLS_BUILD" --parallel "$JOBS"
DESTDIR="$MBEDTLS_ROOT" cmake --install "$MBEDTLS_BUILD"

require_file "$MBEDTLS_ROOT/usr/include/mbedtls/ssl.h" "mbedTLS headers were not installed"
require_file "$MBEDTLS_ROOT/usr/lib/libmbedtls.a" "libmbedtls static library was not installed"
require_file "$MBEDTLS_ROOT/usr/lib/libmbedx509.a" "libmbedx509 static library was not installed"
require_file "$MBEDTLS_ROOT/usr/lib/libmbedcrypto.a" "libmbedcrypto static library was not installed"

# Build a guest-facing tool against the freshly staged static libs, then verify
# it linked fully statically (no interpreter, no NEEDED entries).
build_tool() {
    local name="$1" source="$2" output="$3"
    "$MUSL_CC" -std=c11 -Wall -Wextra -Werror $COMMON_CFLAGS \
        -I"$MBEDTLS_ROOT/usr/include" \
        -DTUNIX_MBEDTLS_VERSION=\"$mbedtls_version\" \
        "$source" \
        "$MBEDTLS_ROOT/usr/lib/libmbedtls.a" \
        "$MBEDTLS_ROOT/usr/lib/libmbedx509.a" \
        "$MBEDTLS_ROOT/usr/lib/libmbedcrypto.a" \
        $COMMON_LDFLAGS -o "$output"
    chmod 0755 "$output"
    if command -v readelf >/dev/null 2>&1; then
        if readelf -l "$output" | grep -q 'INTERP'; then
            fail "$name unexpectedly contains a dynamic interpreter"
        fi
        if readelf -d "$output" 2>/dev/null | grep -q 'NEEDED'; then
            fail "$name unexpectedly contains dynamic dependencies"
        fi
    fi
    return 0
}

build_tool https-get "$HTTPS_GET_SOURCE" "$HTTPS_GET_TOOL"
build_tool ssl-helper "$SSL_HELPER_SOURCE" "$SSL_HELPER_TOOL"

mkdir -p "$MBEDTLS_ROOT/usr/share/licenses/mbedtls"
[[ -f "$MBEDTLS_SOURCE/LICENSE" ]] && cp "$MBEDTLS_SOURCE/LICENSE" "$MBEDTLS_ROOT/usr/share/licenses/mbedtls/"
printf 'mbedtls=%s\n' "$mbedtls_version" > "$MBEDTLS_ROOT/usr/share/tunix-mbedtls.version"

printf 'mbedTLS ready: %s (static musl); https-get + ssl-helper linked\n' "$mbedtls_version"
