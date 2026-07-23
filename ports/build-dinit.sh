#!/usr/bin/env bash
set -euo pipefail

# Build dinit for Tunix.
#
# dinit replaces the hand-rolled /sbin/init: it is a real service manager, so
# the boot sequence (filesystem setup, console keymap, weston via startx)
# becomes declarative service files under /etc/dinit.d instead of C code.
#
# Two decisions that are not obvious from the upstream build:
#
#   * Everything is linked **statically**. PID 1 must never fail to start
#     because the dynamic loader or libstdc++.so went missing from the image;
#     a static musl binary has no runtime dependencies at all. It also means
#     the build can be smoke-tested on the host -- same syscall ABI, no loader.
#   * Every Linux nicety is disabled: cgroups, capabilities, ioprio, oom-adj
#     (Tunix has none of those interfaces) and utmpx (musl only has stubs).
#
# dinit builds in-tree (configure writes ./mconfig next to the sources), so the
# tree is copied into $OUT/dinit-build to keep the submodule pristine.
#
# Output layout (upstream's: only shutdown and friends go to sbin):
#   $OUT/dinit-root/usr/bin    dinit, dinitctl, dinit-check, dinit-monitor
#   $OUT/dinit-root/sbin       shutdown, halt, poweroff, reboot, soft-reboot

PORT_NAME=dinit
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=${OUT:-$ROOT/ports/out}

# shellcheck source=ports/lib/cross-port.sh
source "$ROOT/ports/lib/cross-port.sh"

SOURCE="$ROOT/ports/src/dinit"
BUILD="$OUT/dinit-build"
ROOT_DIR="$OUT/dinit-root"
PATCH_DIR="$ROOT/ports/src/patches/dinit"

EXPECTED_VERSION=0.22.1

[[ -f "$SOURCE/configure" ]] || cross_port_fail \
    "missing dinit source at $SOURCE; run git submodule update --init --recursive"

cross_port_require_toolchain
cross_port_require_tools make m4 "$READELF"

version=$(sed -n 's/^VERSION=//p' "$SOURCE/build/version.conf")
[[ "$version" == "$EXPECTED_VERSION" ]] || \
    cross_port_fail "expected dinit $EXPECTED_VERSION, found ${version:-unknown}"

rm -rf "$BUILD" "$ROOT_DIR"
mkdir -p "$(dirname "$BUILD")"
cp -a "$SOURCE" "$BUILD"
mkdir -p "$ROOT_DIR"

# Patch the copy, never ports/src. Tunix binds unix sockets into a kernel table
# without creating a filesystem node, so dinit's post-bind chmod() of the
# control socket gets ENOENT and would abort startup; the patch tolerates it.
patches=("$PATCH_DIR"/*.patch)
[[ -e "${patches[0]}" ]] || cross_port_fail "no patches found in $PATCH_DIR"
for patch in "${patches[@]}"; do
    # patch(1) rather than `git apply`: the build copy is not a git repository.
    # --fuzz=0 makes a patch that no longer matches an error rather than a
    # silent mis-application; --forward makes a re-run fail fast.
    patch -p1 -d "$BUILD" --fuzz=0 --forward < "$patch" || \
        cross_port_fail "failed to apply $(basename "$patch"); it has probably drifted from dinit $EXPECTED_VERSION"
done

cd "$BUILD"

./configure --quiet --platform=Linux \
    --prefix=/usr \
    --sbindir=/sbin \
    --syscontrolsocket=/run/dinitctl \
    --enable-shutdown \
    --disable-cgroups \
    --disable-capabilities \
    --disable-ioprio \
    --disable-oom-adj \
    --disable-utmpx \
    CXX="$CROSS_CXX" \
    CXX_FOR_BUILD=g++ \
    CXXFLAGS="-std=c++11 -Os -Wall -fno-rtti" \
    LDFLAGS="-static"

make -C build all -j "$JOBS"
make -C src all -j "$JOBS"
make -C src install DESTDIR="$ROOT_DIR"

for binary in usr/bin/dinit usr/bin/dinit-check usr/bin/dinitctl sbin/shutdown; do
    [[ -x "$ROOT_DIR/$binary" ]] || cross_port_fail "$binary was not installed"
done

# A dynamic dinit would boot-loop the machine the day its loader or libstdc++
# vanishes from the image; catch that here, not at boot.
if "$READELF" -l "$ROOT_DIR/usr/bin/dinit" | grep -q INTERP; then
    cross_port_fail "dinit is dynamically linked; PID 1 must be static"
fi

# Static musl binaries run on the build host (same syscall ABI, no loader),
# so the port can prove the binary executes before it ever reaches the image.
# Captured whole, not piped into head: under pipefail, head exiting early
# would turn the check itself into a SIGPIPE failure.
reported=$("$ROOT_DIR/usr/bin/dinit" --version)
[[ "$reported" == *"$EXPECTED_VERSION"* ]] || \
    cross_port_fail "dinit --version reported '$reported'"

rm -rf "$ROOT_DIR/usr/share"

printf 'dinit %s staged at %s\n' "$version" "$ROOT_DIR"
