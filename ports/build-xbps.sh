#!/usr/bin/env bash
set -euo pipefail

# Build xbps, the Void Linux package manager, for Tunix.
#
# A tarball port rather than a submodule, and not by preference: xbps carries
# its repository signing key as data/60:ae:...plist, and a colon cannot appear
# in a filename on NTFS. A submodule checkout of it fails outright on a Windows
# clone of this repository. Unpacking into /var/tmp -- the WSL filesystem, as
# the gnutls and gmp ports already do -- sidesteps that entirely.
#
# The key is then *not* staged for the image, for the same reason: ports/out and
# build/rootfs are on the Windows side too. Nothing is lost by it. xbps offers
# to import a repository's key on first use and writes it under whichever root
# it was given, so an `xbps-install -r /void` puts it in /void/var/db/xbps/keys
# on the Tunix ext2 filesystem, where a colon is an ordinary character.
#
# Output layout:
#   $OUT/xbps-root/usr/bin     xbps-install, xbps-query and the rest
#   $OUT/xbps-root/usr/lib     libxbps
#   $OUT/xbps-root/usr/share   the default xbps.d configuration

PORT_NAME=xbps
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=${OUT:-$ROOT/ports/out}

# shellcheck source=ports/lib/cross-port.sh
source "$ROOT/ports/lib/cross-port.sh"

XBPS_VERSION=0.60.7
XBPS_SHA256=ec8c2e4d595863b5748c10d9ada1d51ac1da6f910a9d31682acd1a477310c64e
XBPS_URL="https://github.com/void-linux/xbps/archive/refs/tags/$XBPS_VERSION.tar.gz"

CACHE=${XBPS_CACHE:-/var/tmp/tunix-xbps}
TARBALL="$CACHE/xbps-$XBPS_VERSION.tar.gz"
SOURCE="$CACHE/xbps-$XBPS_VERSION"
ROOT_DIR="$OUT/xbps-root"

cross_port_require_toolchain
cross_port_require_tools make curl sha256sum tar pkg-config "$READELF"

for module in libarchive libssl libcrypto; do
    [[ -f "$GRAPHICS_SYSROOT/usr/lib/pkgconfig/$module.pc" ]] || cross_port_fail \
        "$module is not in the graphics sysroot; build its port first"
done

mkdir -p "$CACHE"
if [[ ! -f "$TARBALL" ]]; then
    curl -sSL --max-time 600 -o "$TARBALL.partial" "$XBPS_URL" || \
        cross_port_fail "could not download $XBPS_URL"
    mv "$TARBALL.partial" "$TARBALL"
fi

observed=$(sha256sum "$TARBALL" | cut -d' ' -f1)
[[ "$observed" == "$XBPS_SHA256" ]] || cross_port_fail \
    "xbps-$XBPS_VERSION.tar.gz has SHA-256 $observed, expected $XBPS_SHA256"

rm -rf "$SOURCE" "$ROOT_DIR"
mkdir -p "$ROOT_DIR"
tar -xf "$TARBALL" -C "$CACHE"
[[ -x "$SOURCE/configure" ]] || cross_port_fail "the tarball did not unpack a configure script"

cross_port_export_pkg_config
cross_port_autotools_setup

(
    cd "$SOURCE"
    # x86_64-unknown-linux-musl, not the toolchain's own x86_64-linux-musl:
    # configure derives the OS by counting the fields of the triple, and the
    # three-field form makes it read "musl" as the operating system -- which
    # silently drops the Linux branch that defines _XOPEN_SOURCE and
    # _FILE_OFFSET_BITS. CC is already exported, so nothing looks the triple up
    # as a compiler prefix.
    ./configure \
        --host=x86_64-unknown-linux-musl \
        --prefix=/usr \
        --sysconfdir=/etc \
        --localstatedir=/var \
        > configure.log 2>&1 || { tail -40 configure.log; exit 1; }
    make -j "$JOBS" > build.log 2>&1 || { tail -60 build.log; exit 1; }
)

make -C "$SOURCE" install DESTDIR="$ROOT_DIR" > /dev/null

cross_port_check_library "$ROOT_DIR/usr/lib/libxbps.so.6" "libxbps.so.6"
for tool in xbps-install xbps-query xbps-remove xbps-pkgdb xbps-reconfigure; do
    [[ -x "$ROOT_DIR/usr/bin/$tool" ]] || cross_port_fail "$tool was not installed"
done

# See the header: the signing key cannot live on a Windows filesystem, and an
# `-r` install imports its own copy anyway.
rm -rf "$ROOT_DIR/var/db/xbps/keys"
rm -rf "$ROOT_DIR/usr/include" "$ROOT_DIR/usr/lib/pkgconfig" \
    "$ROOT_DIR/usr/share/zsh" "$ROOT_DIR/usr/share/licenses"
find "$ROOT_DIR/usr/lib" -maxdepth 1 -name '*.a' -delete
find "$ROOT_DIR/usr/lib" -maxdepth 1 -type l -name '*.so' -delete

cross_port_finalize_root "$ROOT_DIR"
find "$ROOT_DIR/usr/bin" -type f -exec "$CROSS_STRIP" --strip-all {} + 2>/dev/null || true
cross_port_check_runtime_closure "$ROOT_DIR" "$OUT/libarchive-root" "$OUT/zstd-root" \
    "$OUT/openssl-root" "$OUT/musl-shared-root" "$OUT/image-codecs-shared-root"
