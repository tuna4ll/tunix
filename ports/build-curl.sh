#!/usr/bin/env bash
set -euo pipefail

# Build libcurl as a static musl library against the already-ported mbedTLS,
# so git's git-remote-https helper has an https:// transport. Mirrors
# build-mbedtls.sh / build-image-codecs.sh: CMake + the shared musl toolchain,
# staged under ports/out. Only libcurl (the library) is built; the curl(1)
# command-line tool is intentionally left out -- git links the library.

ROOT=$(cd "$(dirname "$0")/.." && pwd)
# Reuse the shared musl toolchain setup; curl is not autotools here (we drive
# its CMake), so no gnu_autotools_port.
source "$ROOT/ports/lib/gnu-port.sh"

PORT_NAME=curl
SRC="$ROOT/ports/src/curl"
BUILD="$OUT/curl-build"
ROOT_DIR="$OUT/curl-root"
MBEDTLS_ROOT="$OUT/mbedtls-root"

[[ -f "$SRC/CMakeLists.txt" ]] || gnu_port_fail "missing curl source; run git submodule update --init --recursive"
# curl's one dependency here is mbedTLS, which its own port stages. The Makefile
# orders the mbedtls stamp before this script, but assert it so a hand-run of
# this script fails loudly rather than silently building an SSL-less libcurl.
[[ -f "$MBEDTLS_ROOT/usr/lib/libmbedtls.a" ]] || \
    gnu_port_fail "mbedTLS static libraries not found at $MBEDTLS_ROOT; build mbedtls first"
[[ -f "$MBEDTLS_ROOT/usr/include/mbedtls/ssl.h" ]] || \
    gnu_port_fail "mbedTLS headers not found; build mbedtls first"

for tool in cmake make; do
    command -v "$tool" >/dev/null 2>&1 || gnu_port_fail "$tool was not found"
done

gnu_port_detect_flags
gnu_port_ensure_toolchain

curl_version=$(sed -n 's/^#define LIBCURL_VERSION[[:space:]]*"\([^"]*\)".*/\1/p' "$SRC/include/curl/curlver.h" | head -n1)
[[ -n "$curl_version" ]] || gnu_port_fail "could not determine the libcurl version"

rm -rf "$BUILD" "$ROOT_DIR"
mkdir -p "$BUILD" "$ROOT_DIR"

# A note on what is deliberately turned off, and why:
#   * BUILD_SHARED_LIBS/BUILD_CURL_EXE OFF, BUILD_STATIC_LIBS ON: the image is
#     fully static and we ship the library, not the tool.
#   * TLS: mbedTLS only. OpenSSL/GnuTLS/wolfSSL/Rustls off -- none are ported,
#     and letting CMake probe the host could link the host's OpenSSL.
#   * Every optional codec/resolver library (zlib, brotli, zstd, nghttp2,
#     libidn2, libpsl, c-ares, GSSAPI, libssh) OFF: none are in the sysroot.
#     Without CURL_USE_LIBPSL=OFF the configure step *errors* rather than
#     silently continuing. zlib off only means curl won't do transfer-encoding
#     gzip; git does its own object compression, so http transport is unaffected.
#   * Threaded resolver OFF: matches git's NO_PTHREADS posture and keeps the
#     synchronous getaddrinfo path, which is all the kernel resolver needs.
#   * IPv6 OFF: the Tunix net stack is IPv4-only, and an AF_INET6 socket() would
#     just fail; disabling it avoids curl trying and logging errors.
#   * Protocols reduced to HTTP(S): git only ever drives http/https through the
#     remote-http helper. HTTP_ONLY would also kill the tool's other protocols,
#     but we keep FILE too since it is free and harmless.
#   * CA bundle baked to the guest path /etc/ssl/cert.pem, which the mbedtls
#     port already installs into the rootfs. mbedTLS has no default CA store, so
#     without this git could not verify any certificate.
# CMAKE_USE_PKGCONFIG/CMAKECONFIG OFF stop CMake from discovering host copies of
# any of the above.
cmake -S "$SRC" -B "$BUILD" -G "Unix Makefiles" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER="$MUSL_CC" \
    -DCMAKE_AR="$(command -v "$HOST_AR")" \
    -DCMAKE_RANLIB="$(command -v "$HOST_RANLIB")" \
    -DCMAKE_INSTALL_PREFIX=/usr \
    -DCMAKE_INSTALL_LIBDIR=lib \
    -DCMAKE_C_FLAGS="$COMMON_CFLAGS" \
    -DCMAKE_EXE_LINKER_FLAGS="$COMMON_LDFLAGS" \
    -DCMAKE_POSITION_INDEPENDENT_CODE=OFF \
    -DBUILD_SHARED_LIBS=OFF \
    -DBUILD_STATIC_LIBS=ON \
    -DBUILD_CURL_EXE=OFF \
    -DCURL_USE_PKGCONFIG=OFF \
    -DCURL_USE_CMAKECONFIG=OFF \
    -DCURL_ENABLE_SSL=ON \
    -DCURL_USE_MBEDTLS=ON \
    -DCURL_USE_OPENSSL=OFF \
    -DCURL_USE_GNUTLS=OFF \
    -DCURL_USE_WOLFSSL=OFF \
    -DCURL_USE_RUSTLS=OFF \
    -DCURL_DEFAULT_SSL_BACKEND=mbedtls \
    -DMBEDTLS_INCLUDE_DIR="$MBEDTLS_ROOT/usr/include" \
    -DMBEDTLS_LIBRARY="$MBEDTLS_ROOT/usr/lib/libmbedtls.a" \
    -DMBEDX509_LIBRARY="$MBEDTLS_ROOT/usr/lib/libmbedx509.a" \
    -DMBEDCRYPTO_LIBRARY="$MBEDTLS_ROOT/usr/lib/libmbedcrypto.a" \
    -DCURL_ZLIB=OFF \
    -DCURL_BROTLI=OFF \
    -DCURL_ZSTD=OFF \
    -DUSE_NGHTTP2=OFF \
    -DUSE_LIBIDN2=OFF \
    -DCURL_USE_LIBPSL=OFF \
    -DCURL_USE_GSSAPI=OFF \
    -DCURL_USE_LIBSSH=OFF \
    -DCURL_USE_LIBSSH2=OFF \
    -DENABLE_ARES=OFF \
    -DENABLE_THREADED_RESOLVER=OFF \
    -DENABLE_IPV6=OFF \
    -DCURL_DISABLE_LDAP=ON \
    -DCURL_DISABLE_LDAPS=ON \
    -DCURL_DISABLE_DICT=ON \
    -DCURL_DISABLE_GOPHER=ON \
    -DCURL_DISABLE_IMAP=ON \
    -DCURL_DISABLE_MQTT=ON \
    -DCURL_DISABLE_POP3=ON \
    -DCURL_DISABLE_RTSP=ON \
    -DCURL_DISABLE_SMTP=ON \
    -DCURL_DISABLE_TELNET=ON \
    -DCURL_DISABLE_TFTP=ON \
    -DCURL_DISABLE_FTP=ON \
    -DCURL_CA_BUNDLE="/etc/ssl/cert.pem" \
    -DCURL_CA_PATH="none" \
    -DBUILD_LIBCURL_DOCS=OFF \
    -DBUILD_MISC_DOCS=OFF \
    -DENABLE_CURL_MANUAL=OFF \
    -DCURL_DISABLE_INSTALL=OFF

cmake --build "$BUILD" --parallel "$JOBS"
DESTDIR="$ROOT_DIR" cmake --install "$BUILD"

[[ -f "$ROOT_DIR/usr/lib/libcurl.a" ]] || gnu_port_fail "libcurl static library was not installed"
[[ -f "$ROOT_DIR/usr/include/curl/curl.h" ]] || gnu_port_fail "libcurl headers were not installed"

# git's Makefile probes CURL_CONFIG for a version number. curl-config is a
# shell script CMake generates; make sure it is present and executable so the
# git port does not have to special-case a missing one.
[[ -f "$ROOT_DIR/usr/bin/curl-config" ]] || gnu_port_fail "curl-config was not installed"
chmod 0755 "$ROOT_DIR/usr/bin/curl-config"

# Confirm curl actually compiled its mbedTLS backend in: with CURL_ENABLE_SSL=ON
# but no usable backend, curl can silently build an SSL-less libcurl whose only
# symptom is a runtime "Protocol https not supported". curl_config.h is the
# authoritative, generated record of what was enabled -- USE_MBEDTLS is defined
# only when the backend is really wired up. (An earlier revision grepped the
# static archive with nm, but under `set -o pipefail` nm's non-zero exit on the
# empty object members that libcurl.a contains masked a genuine match and gave a
# false negative; the generated header avoids the whole toolchain dependency.)
curl_config_h="$BUILD/lib/curl_config.h"
[[ -f "$curl_config_h" ]] || gnu_port_fail "curl_config.h was not generated; cannot confirm the TLS backend"
grep -q '^#define USE_MBEDTLS 1' "$curl_config_h" || \
    gnu_port_fail "curl did not enable its mbedTLS backend (USE_MBEDTLS missing from curl_config.h); https would not work"

echo "libcurl $curl_version root staged at $ROOT_DIR (static, mbedTLS backend)"
