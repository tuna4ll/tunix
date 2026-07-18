#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
# Reuse the shared musl toolchain setup; iproute2 itself is not autotools, so
# we drive its bespoke configure/make by hand rather than gnu_autotools_port.
source "$ROOT/ports/lib/gnu-port.sh"

PORT_NAME=iproute2
SRC="$ROOT/ports/src/iproute2"
BUILD="$OUT/iproute2-build"
ROOT_DIR="$OUT/iproute2-root"

[[ -f "$SRC/Makefile" ]] || gnu_port_fail "missing iproute2 source; run git submodule update --init --recursive"

gnu_port_detect_flags
gnu_port_ensure_toolchain

# iproute2 builds in-tree and bundles its own copy of the Linux UAPI headers
# under include/uapi, so it needs no kernel-headers in the sysroot. Build from a
# throwaway copy to keep the submodule checkout pristine across rebuilds.
rm -rf "$BUILD" "$ROOT_DIR"
mkdir -p "$ROOT_DIR"
cp -a "$SRC" "$BUILD"

(
    cd "$BUILD"
    # PKG_CONFIG=/bin/false makes every optional-library probe (libmnl, libelf,
    # libcap, libbsd, libselinux, libbpf) fail, so configure disables them and
    # ip/ss fall back to their built-in libnetlink paths -- exactly what a fully
    # static musl build needs.
    # Every feature probe in iproute2's configure *links* a test binary with a
    # bare "$CC -o ...", ignoring CFLAGS/LDFLAGS entirely. Against our static
    # musl libc a default-PIE link fails ("relocation R_X86_64_32 ... can not be
    # used when making a PIE object"), so without these flags baked into CC each
    # probe fails for the wrong reason and configure concludes the libc lacks
    # setns, strlcpy, etc. iproute2 then emits its own fallback definitions,
    # which collide with musl's real declarations at compile time.
    env CC="$MUSL_CC -fno-pie -no-pie $NO_AUTO_ATOMIC" PKG_CONFIG=/bin/false LIBBPF_FORCE=off \
        ./configure || gnu_port_fail "iproute2 configure failed"

    # Build only what we ship: the internal libraries, ip, and ss. Skipping tc,
    # rdma, devlink, man, etc. avoids pulling in features that need libraries the
    # static sysroot does not carry.
    #
    # ss must be built by recursing from the top level rather than with a direct
    # "make -C misc ss": the -I../include search path and the LDLIBS entry for
    # lib/libnetlink.a are defined only in the top-level Makefile, and misc's own
    # Makefile pulls in config.mk but never those. Going through SUBDIRS also
    # builds nstat/ifstat/rtacct/lnstat, which are small and self-contained; we
    # simply do not stage them.
    make -j"$JOBS" \
        CC="$MUSL_CC" \
        CCOPTS="-Os -fno-stack-protector -fno-pie $NO_AUTO_ATOMIC" \
        LDFLAGS="-static -no-pie $NO_AUTO_ATOMIC" \
        SUBDIRS="lib ip misc"
)

[[ -x "$BUILD/ip/ip" ]] || gnu_port_fail "ip binary was not built"
[[ -x "$BUILD/misc/ss" ]] || gnu_port_fail "ss binary was not built"

mkdir -p "$ROOT_DIR/usr/sbin" "$ROOT_DIR/etc/iproute2"
cp "$BUILD/ip/ip" "$ROOT_DIR/usr/sbin/ip"
cp "$BUILD/misc/ss" "$ROOT_DIR/usr/sbin/ss"
chmod 0755 "$ROOT_DIR/usr/sbin/ip" "$ROOT_DIR/usr/sbin/ss"

# ip reads these tables to translate table/protocol/scope numbers into names;
# without them it still works but only prints numeric ids.
if [[ -d "$BUILD/etc/iproute2" ]]; then
    cp -R "$BUILD/etc/iproute2/." "$ROOT_DIR/etc/iproute2/"
fi

echo "iproute2 root staged at $ROOT_DIR (ip, ss)"
