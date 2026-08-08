#!/usr/bin/env bash
set -euo pipefail

# Build OpenSSL for Tunix. xbps verifies repository signatures with RSA and
# fetches over HTTPS through its bundled libfetch, and both of those are
# OpenSSL's API specifically -- the mbedTLS and GnuTLS already on the image
# cannot stand in for it.
#
# Libraries only, deliberately: `no-apps`. The image already has a
# /usr/bin/openssl, and it is not this one -- it is the mbedTLS-backed
# `s_client` shim from tools/ssl-helper.c that HTTPS clients shell out to.
# Installing the real tool over it would replace something that already works
# for a program nothing has asked for.
#
# --openssldir=/etc/ssl matches the image, so OpenSSL's default CA file is the
# /etc/ssl/cert.pem bundle that is already installed there.
#
# Output layout:
#   $OUT/graphics-sysroot/usr/{include,lib}   headers + .pc for libarchive/xbps
#   $OUT/openssl-root/usr/lib                 libcrypto/libssl for the image

PORT_NAME=openssl
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=${OUT:-$ROOT/ports/out}

# shellcheck source=ports/lib/cross-port.sh
source "$ROOT/ports/lib/cross-port.sh"

OPENSSL_SOURCE="$ROOT/ports/src/openssl"
# Out of tree and off drvfs: OpenSSL's build is thousands of small files and
# runs several times faster on the WSL filesystem than under /mnt/c.
BUILD=${OPENSSL_BUILD_DIR:-/var/tmp/tunix-openssl-build}
ROOT_DIR="$OUT/openssl-root"

EXPECTED_OPENSSL_VERSION=3.6.3

[[ -f "$OPENSSL_SOURCE/Configure" ]] || cross_port_fail \
    "missing $OPENSSL_SOURCE; run git submodule update --init ports/src/openssl"

cross_port_require_toolchain
cross_port_require_tools make perl "$READELF"

openssl_version=$(sed -n 's/^\(MAJOR\|MINOR\|PATCH\)=//p' "$OPENSSL_SOURCE/VERSION.dat" | paste -sd. -)
[[ "$openssl_version" == "$EXPECTED_OPENSSL_VERSION" ]] || \
    cross_port_fail "expected openssl $EXPECTED_OPENSSL_VERSION, found ${openssl_version:-unknown}"

rm -rf "$BUILD" "$ROOT_DIR"
mkdir -p "$BUILD" "$ROOT_DIR"

cross_port_export_pkg_config

(
    cd "$BUILD"
    # linux-x86_64 with a cross-compile prefix is OpenSSL's own cross idiom; it
    # never runs a target binary during configuration, so no exe wrapper is
    # needed. no-docs keeps the perl pod toolchain out of the build.
    perl "$OPENSSL_SOURCE/Configure" linux-x86_64 \
        --cross-compile-prefix="$CROSS_DIR/bin/$CROSS_TARGET-" \
        --prefix=/usr \
        --libdir=lib \
        --openssldir=/etc/ssl \
        shared \
        no-apps \
        no-docs \
        no-tests \
        > configure.log 2>&1 || { tail -40 configure.log; exit 1; }
    make -j "$JOBS" > build.log 2>&1 || { tail -60 build.log; exit 1; }
)

# install_sw is the libraries, headers and pkg-config data; install_ssldirs
# would add an /etc/ssl skeleton that would overwrite the image's CA bundle.
make -C "$BUILD" install_sw DESTDIR="$GRAPHICS_SYSROOT" > /dev/null
make -C "$BUILD" install_sw DESTDIR="$ROOT_DIR" > /dev/null

[[ -f "$GRAPHICS_SYSROOT/usr/lib/pkgconfig/libcrypto.pc" ]] || \
    cross_port_fail "libcrypto.pc was not installed"

cross_port_check_library "$ROOT_DIR/usr/lib/libcrypto.so.3" "libcrypto.so.3"
cross_port_check_library "$ROOT_DIR/usr/lib/libssl.so.3" "libssl.so.3"

rm -rf "$ROOT_DIR/usr/include" "$ROOT_DIR/usr/lib/pkgconfig" "$ROOT_DIR/usr/lib/cmake" \
    "$ROOT_DIR/usr/share"
find "$ROOT_DIR/usr/lib" -name '*.a' -delete
find "$ROOT_DIR/usr/lib" -maxdepth 1 -type l -name '*.so' -delete

cross_port_finalize_root "$ROOT_DIR"
cross_port_check_runtime_closure "$ROOT_DIR" "$OUT/musl-shared-root"
