#!/usr/bin/env bash
set -euo pipefail

# Build an x86_64-linux-musl cross toolchain (gcc, g++, libstdc++) for Tunix.
#
# Why this exists: every other port compiles with musl's musl-gcc wrapper, which
# is a C-only shim around the host gcc.  It cannot build C++, because the host's
# libstdc++ headers and library are configured for glibc -- <bits/os_defines.h>
# calls __GLIBC_PREREQ and <bits/c++locale.h> wants glibc's __locale_t, so the
# very first #include fails against a musl sysroot.  Mesa mandates C++17
# (project('mesa', ['c', 'cpp'], cpp_std=c++17)), so a real cross toolchain with
# a musl-targeted libstdc++ is a hard prerequisite for the graphics ports.
#
# The heavy lifting is musl-cross-make (ports/src/musl-cross-make), which builds
# binutils + gcc + musl in the correct order and carries the target patches.  It
# downloads the upstream release tarballs on the first build; they are cached in
# $OUT/musl-cross-dl so later rebuilds stay offline.
#
# Output layout:
#   $OUT/musl-cross/bin/x86_64-linux-musl-{gcc,g++,ar,ld,...}
#   $OUT/musl-cross/x86_64-linux-musl/          target sysroot (libc, libstdc++)
#
# This is a build-host toolchain, not something we ship: nothing here is staged
# into the initramfs.  The libraries it links against *are* shipped, by the
# ports that use it.

ROOT=$(cd "$(dirname "$0")/.." && pwd)
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)}
OUT=${OUT:-$ROOT/ports/out}

MCM_SOURCE="$ROOT/ports/src/musl-cross-make"
CROSS="$OUT/musl-cross"
DOWNLOADS="$OUT/musl-cross-dl"
# Building gcc across a 9p/drvfs mount (a Windows drive under WSL) is roughly an
# order of magnitude slower than on a native filesystem, and this is the one
# port where that turns minutes into hours. Allow an override so a developer can
# point the scratch build at a native path; the installed toolchain still lands
# under $OUT either way, so nothing else in the tree cares.
BUILD=${MUSL_CROSS_BUILD_DIR:-$OUT/musl-cross-build}

TARGET=x86_64-linux-musl
# Pinned so the toolchain is reproducible. MUSL_VER deliberately matches the
# version of the ports/src/musl submodule (see its VERSION file) so the cross
# toolchain and the musl-gcc ports agree on libc.
BINUTILS_VER=2.44
GCC_VER=15.1.0
MUSL_VER=1.2.6

CROSS_CC="$CROSS/bin/$TARGET-gcc"
CROSS_CXX="$CROSS/bin/$TARGET-g++"
SYSROOT="$CROSS/$TARGET"
# Bump the suffix whenever the pinned versions or the configure flags change, so
# a stale toolchain is rebuilt instead of silently reused.
STAMP="$CROSS/.tunix-musl-cross-v1-$GCC_VER-$BINUTILS_VER-$MUSL_VER"

READELF=${READELF:-readelf}

fail() {
    echo "build-musl-cross: $*" >&2
    exit 1
}

# shellcheck source=ports/lib/kernel-headers.sh
source "$ROOT/ports/lib/kernel-headers.sh"

[[ -f "$MCM_SOURCE/Makefile" ]] || \
    fail "missing musl-cross-make source at $MCM_SOURCE; run git submodule update --init --recursive"
[[ -d "$MCM_SOURCE/patches/gcc-$GCC_VER" ]] || \
    fail "musl-cross-make has no patches for gcc $GCC_VER; pick a version under $MCM_SOURCE/patches"
[[ -f "$MCM_SOURCE/hashes/musl-$MUSL_VER.tar.gz.sha1" ]] || \
    fail "musl-cross-make has no hash for musl $MUSL_VER"

for tool in make gcc g++ sha1sum tar bzip2 xz "$READELF"; do
    command -v "$tool" >/dev/null 2>&1 || fail "$tool was not found"
done

# musl-cross-make defaults to wget; fall back to curl so a host with only one of
# them still works.
if command -v wget >/dev/null 2>&1; then
    DL_CMD="wget -c -O"
elif command -v curl >/dev/null 2>&1; then
    DL_CMD="curl -C - -L -o"
else
    fail "either wget or curl is required to download the toolchain sources"
fi

if [[ -f "$STAMP" && -x "$CROSS_CXX" ]]; then
    echo "build-musl-cross: reusing $($CROSS_CXX -dumpversion) toolchain at $CROSS"
else
    rm -rf "$CROSS"
    mkdir -p "$BUILD" "$CROSS" "$DOWNLOADS"

    # Build out of tree: musl-cross-make unpacks tarballs and builds next to its
    # own Makefile, so we work on a copy and leave ports/src pristine.
    tar -C "$MCM_SOURCE" --exclude=.git -cf - . | tar -C "$BUILD" -xf -

    # Drop only the compiled output, not the unpacked-and-patched source trees:
    # re-extracting the gcc tarball costs minutes on every retry, and musl-cross-
    # make treats those directories as order-only prerequisites, so keeping them
    # is safe. The generated config.mak below is picked up either way, because
    # removing build/ forces litecross to regenerate its own copy of it.
    rm -rf "$BUILD/build"

    # Point the in-tree download directory at the persistent cache. Passing
    # SOURCES=<external path> instead would make musl-cross-make treat it as
    # immutable and refuse to download into it, so a symlink is the way to get
    # both caching and downloading.
    rm -rf "$BUILD/sources"
    ln -s "$DOWNLOADS" "$BUILD/sources"

    cat > "$BUILD/config.mak" <<EOF_CONFIG
TARGET = $TARGET
BINUTILS_VER = $BINUTILS_VER
GCC_VER = $GCC_VER
MUSL_VER = $MUSL_VER
DL_CMD = $DL_CMD

# musl-cross-make defaults to ftpmirror.gnu.org, which round-robins to whatever
# GNU mirror is nearest; several of those are unreliable and a dead one fails
# the whole build. Point at the canonical host instead.
GNU_SITE = https://ftp.gnu.org/gnu

# Blank: musl-cross-make would otherwise install a 4.19 UAPI snapshot, which is
# far too old for libdrm and mesa. We stage the host's kernel headers into the
# sysroot after the install instead, exactly like ports/lib/gnu-port.sh does.
LINUX_VER =

COMMON_CONFIG += --disable-nls
# -fno-char8_t is not cosmetic: gcc 15's libcody writes its diagnostics as u8""
# literals, which are const char[] in C++17 but const char8_t[] from C++20 on,
# so a host g++ defaulting to C++20 or later fails with "cannot convert
# 'const char8_t*' to 'char'". Pinning -std=gnu++17 instead would look like the
# obvious fix but breaks libcody's configure, which insists on __cplusplus being
# exactly 201103 and appends its own -std=c++11 that our flag would override.
COMMON_CONFIG += CFLAGS="-g0 -O2" CXXFLAGS="-g0 -O2 -fno-char8_t" LDFLAGS="-s"

GCC_CONFIG += --enable-languages=c,c++
# Trim what neither mesa nor libdrm uses; this is a large chunk of the build.
GCC_CONFIG += --disable-libquadmath --disable-decimal-float
GCC_CONFIG += --disable-libitm --disable-fixed-point
GCC_CONFIG += --disable-libsanitizer --disable-lto
EOF_CONFIG

    (
        cd "$BUILD"
        make -j"$JOBS"
        make install OUTPUT="$CROSS"
    )

    [[ -x "$CROSS_CC" ]] || fail "$TARGET-gcc was not installed"
    [[ -x "$CROSS_CXX" ]] || fail "$TARGET-g++ was not installed"
    : > "$STAMP"
fi

tunix_install_kernel_headers "$SYSROOT/include" fail

# The libstdc++ that the C++ ports will link against has to be there, both as a
# shared object for the runtime and as a linker symlink for the build.
libstdcxx=$(find "$SYSROOT/lib" -maxdepth 1 -name 'libstdc++.so.*' -print -quit)
[[ -n "$libstdcxx" ]] || fail "libstdc++ was not built for $TARGET"
# In musl, libc.so *is* the dynamic loader; ld-musl-x86_64.so.1 next to it is an
# absolute symlink to /lib/libc.so, which resolves against the build host's root
# rather than the sysroot. Following it here would hand us the host's glibc
# linker script, so always use the real file.
loader="$SYSROOT/lib/libc.so"
[[ -f "$loader" ]] || fail "the target musl runtime is missing from the sysroot"

# End-to-end check: the point of this port is C++, so compile and *run* a C++
# program that exercises the pieces mesa depends on (the STL, exceptions, RTTI
# and threads all pull in libstdc++ runtime support that a half-built toolchain
# would fail on).
probe_dir="$OUT/musl-cross-check"
rm -rf "$probe_dir"
mkdir -p "$probe_dir"
cat > "$probe_dir/check.cpp" <<'EOF_CHECK'
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

int main() {
    std::vector<std::string> parts{"musl", "cross"};
    auto owned = std::make_unique<std::string>("libstdc++");
    int counter = 0;
    std::thread worker([&counter] { counter = 1; });
    worker.join();
    try {
        throw std::runtime_error("exceptions");
    } catch (const std::exception &error) {
        std::printf("%s-%s %s %s %d\n", parts[0].c_str(), parts[1].c_str(),
                    owned->c_str(), error.what(), counter);
    }
    return 0;
}
EOF_CHECK

"$CROSS_CXX" -std=c++17 -O2 -fPIE -pie -pthread \
    "$probe_dir/check.cpp" -o "$probe_dir/check"

interp=$("$READELF" -l "$probe_dir/check" | \
    sed -n 's/.*Requesting program interpreter: \([^]]*\).*/\1/p')
[[ "$interp" == "/lib/ld-musl-x86_64.so.1" ]] || \
    fail "cross-compiled binary has unexpected interpreter '${interp:-missing}'"

needed=$("$READELF" -d "$probe_dir/check" | sed -n 's/.*Shared library: \[\([^]]*\)\].*/\1/p')
grep -Fxq "libstdc++.so.6" <<<"$needed" || fail "check binary does not link libstdc++"

# A musl-target binary uses the same x86_64 Linux syscall ABI as the build host,
# so it runs here as well as on Tunix once given the target loader.
output=$("$loader" --library-path "$SYSROOT/lib" "$probe_dir/check")
[[ "$output" == "musl-cross libstdc++ exceptions 1" ]] || \
    fail "C++ runtime check produced unexpected output: $output"

# Static C++ has to work too: several helper tools are easier to ship static.
"$CROSS_CXX" -std=c++17 -O2 -static "$probe_dir/check.cpp" -pthread \
    -o "$probe_dir/check-static"
if "$READELF" -l "$probe_dir/check-static" | grep -q 'Requesting program interpreter'; then
    fail "static C++ check binary unexpectedly has an interpreter"
fi
"$probe_dir/check-static" >/dev/null

printf 'musl cross toolchain ready: gcc %s, binutils %s, musl %s at %s\n' \
    "$GCC_VER" "$BINUTILS_VER" "$MUSL_VER" "$CROSS"
