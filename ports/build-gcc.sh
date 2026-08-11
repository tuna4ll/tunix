#!/usr/bin/env bash
set -euo pipefail

# Put a real GCC on the image.
#
# Unlike every other port here, nothing is compiled: the compiler is *fetched*,
# as a binary package, from Void Linux's musl repository with our own xbps
# (ports/build-xbps.sh). Building GCC to run on Tunix would be a Canadian cross
# -- build != host -- of the largest source tree in the tree, for a compiler
# Void already publishes for exactly this triple. Tunix's musl is the same
# version as Void's (1.2.6), which is what makes their binaries runnable here at
# all; see docs/package-manager.md.
#
# The one thing that needs care is that Void's gcc is *dynamic* and its
# libraries collide by name with the image's own. So the package tree is staged
# under a private prefix:
#
#   /opt/gcc/usr/bin/gcc                 the driver, as Void built it
#   /opt/gcc/usr/lib/gcc/<triple>/<ver>/ cc1, collect2, crt*.o, libgcc.a
#   /opt/gcc/usr/lib/*.so.*              only the shared objects it needs
#   /usr/bin/gcc                         wrapper: sets LD_LIBRARY_PATH, execs it
#
# GCC finds cc1 and libgcc relative to its own argv[0], so a driver at
# /opt/gcc/usr/bin/gcc looks for the rest of the toolchain under /opt/gcc/usr --
# but the *system* header and library directories are not relocated, so
# /usr/include and /usr/lib are still the image's own musl. That is the split we
# want: Void supplies the compiler, Tunix supplies the libc.
#
# Output layout:
#   $OUT/gcc-root/opt/gcc/       the compiler
#   $OUT/gcc-root/usr/bin/       the wrappers
#   $OUT/gcc-root/usr/lib/       libc.so link, libssp_nonshared.a
#   $OUT/gcc-sysroot/            a copy of what the image's /usr looks like,
#                                used to compile and run a test program here

PORT_NAME=gcc
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=${OUT:-$ROOT/ports/out}

# shellcheck source=ports/lib/cross-port.sh
source "$ROOT/ports/lib/cross-port.sh"

VOID_REPO=${VOID_REPO:-https://repo-default.voidlinux.org/current/musl}
VOID_ARCH=x86_64-musl
GCC_PACKAGE=${GCC_PACKAGE:-gcc}
TARGET_TRIPLE=x86_64-linux-musl

# The staging root must be on a real Linux filesystem: xbps writes the
# repository's public key as <root>/var/db/xbps/keys/60:ae:....plist, and a
# colon cannot appear in a filename on the NTFS drive this tree usually lives
# on. Nothing with a colon in its name is copied out of here.
STAGE=${GCC_STAGE_DIR:-/var/tmp/tunix-void-gcc}
# Kept out of the staging root so that wiping it -- which is what makes a rerun
# deterministic -- does not throw away 80 MiB of downloads.
CACHE=${GCC_CACHE_DIR:-/var/tmp/tunix-void-cache}

XBPS_ROOT="$OUT/xbps-root"
GCC_ROOT="$OUT/gcc-root"
GCC_SYSROOT="$OUT/gcc-sysroot"
MUSL_SYSROOT="$OUT/sysroot"
MUSL_SHARED_ROOT="$OUT/musl-shared-root"
BINUTILS_ROOT="$OUT/binutils-root"

PREFIX=/opt/gcc

cross_port_require_tools find sed tr "$READELF"
[[ -x "$XBPS_ROOT/usr/bin/xbps-install" ]] || cross_port_fail \
    "xbps is missing; run ports/build-xbps.sh first"
[[ -d "$MUSL_SYSROOT/usr/include/sys" ]] || cross_port_fail \
    "the static musl sysroot is missing; run ports/build-tcc.sh first"
[[ -x "$BINUTILS_ROOT/usr/bin/as" ]] || cross_port_fail \
    "binutils is missing; run ports/build-binutils.sh first"
[[ -f "$MUSL_SHARED_ROOT/lib/libc.so" ]] || cross_port_fail \
    "the shared musl runtime is missing; run ports/build-musl-shared.sh first"
[[ -f "$CROSS_LOADER" ]] || cross_port_fail \
    "the musl cross toolchain is missing; run ports/build-musl-cross.sh first"

# Everything below runs musl binaries on the build host. They use the same
# syscall ABI, so all they need is the loader at the absolute path their .interp
# names -- the same shim cross_port_autotools_setup installs.
ln -sf "$CROSS_LOADER" /lib/ld-musl-x86_64.so.1 2>/dev/null || true
[[ -e /lib/ld-musl-x86_64.so.1 ]] || cross_port_fail \
    "/lib/ld-musl-x86_64.so.1 could not be created; run this as root"

# xbps itself is dynamic and its dependencies live in the ports that built them.
XBPS_LIBS="$XBPS_ROOT/usr/lib"
for dir in openssl-root libarchive-root zstd-root image-codecs-shared-root; do
    [[ -d "$OUT/$dir/usr/lib" ]] || cross_port_fail \
        "$dir is missing; xbps cannot run without it"
    XBPS_LIBS="$XBPS_LIBS:$OUT/$dir/usr/lib"
done
XBPS_LIBS="$XBPS_LIBS:$CROSS_SYSROOT/lib"

xbps() {
    local prog="$1"; shift
    LD_LIBRARY_PATH="$XBPS_LIBS" "$XBPS_ROOT/usr/bin/$prog" "$@"
}

# ---------------------------------------------------------------- fetch

rm -rf "$STAGE"
mkdir -p "$STAGE/etc/xbps.d" "$CACHE"
# architecture= is mandatory: uname(2) reports x86_64 and xbps would otherwise
# resolve against the glibc repository. Note the config directory is read from
# *inside* the root, which is why it is written here and not in /etc.
cat > "$STAGE/etc/xbps.d/00-repository-main.conf" <<EOF_CONF
repository=$VOID_REPO
architecture=$VOID_ARCH
EOF_CONF

# -U unpacks without running the packages' INSTALL scripts. Those expect to run
# on the system they are configuring; here the root is a staging directory that
# is about to be taken apart. `yes` answers the public-key import, which -y does
# not cover -- xbps asks for that one unconditionally. pipefail has to come off
# around it, or the SIGPIPE `yes` takes when xbps exits fails the whole script.
echo "build-gcc: fetching $GCC_PACKAGE from $VOID_REPO"
set +o pipefail
yes | xbps xbps-install -r "$STAGE" -c "$CACHE" -y -U -S "$GCC_PACKAGE"
set -o pipefail

GCC_LIBDIR=$(find "$STAGE/usr/lib/gcc/$TARGET_TRIPLE" -mindepth 1 -maxdepth 1 \
    -type d -name '[0-9]*' -exec test -e '{}/cc1' ';' -print | sed -n 1p)
[[ -n "$GCC_LIBDIR" ]] || cross_port_fail "no cc1 under $STAGE/usr/lib/gcc/$TARGET_TRIPLE"
GCC_VERSION=$(basename "$GCC_LIBDIR")
FULL_VERSION=$(xbps xbps-query -r "$STAGE" -p pkgver gcc)
echo "build-gcc: staging $FULL_VERSION (compiler directory $GCC_VERSION)"

# ---------------------------------------------------------------- select

rm -rf "$GCC_ROOT"
mkdir -p "$GCC_ROOT$PREFIX/usr/bin" "$GCC_ROOT$PREFIX/usr/lib/gcc/$TARGET_TRIPLE" \
    "$GCC_ROOT/usr/bin" "$GCC_ROOT/usr/lib"

cp -a "$STAGE/usr/bin/gcc" "$STAGE/usr/bin/cpp" "$GCC_ROOT$PREFIX/usr/bin/"
cp -a "$STAGE/usr/lib/gcc/$TARGET_TRIPLE/." "$GCC_ROOT$PREFIX/usr/lib/gcc/$TARGET_TRIPLE/"

# Void's gcc is configured with a lib64 exec prefix -- it looks for cc1 under
# <prefix>/lib64/gcc/<triple>/<version> -- while the package installs into lib
# and leaves the symlink to base-files, which is not one of gcc's dependencies.
# Without this the driver finds nothing and says "cannot execute 'cc1'".
ln -sfn lib "$GCC_ROOT$PREFIX/usr/lib64"

# The C compiler is 40 MiB on its own; the languages we are not shipping are
# another 130 MiB of initramfs, which is loaded into RAM in one piece. Drop them
# rather than the whole package, because everything else here -- the driver,
# collect2, the startup files, libgcc -- is shared with the C compiler.
#   cc1plus  C++     lto1  -flto     gnat1  Ada     g++-mapper-server  modules
#
# liblto_plugin.so stays even though LTO does not: this driver was configured to
# pass -plugin to the linker on *every* link, and refuses to link at all if the
# plugin is missing. The image's binutils is built with --disable-plugins, which
# accepts the option and ignores it, so ordinary links work and -flto does not.
pruned=(cc1plus lto1 gnat1 g++-mapper-server
        install-tools plugin ada_target_properties)
for name in "${pruned[@]}"; do
    rm -rf "$GCC_ROOT$PREFIX/usr/lib/gcc/$TARGET_TRIPLE/$GCC_VERSION/$name"
done

[[ -x "$GCC_ROOT$PREFIX/usr/lib/gcc/$TARGET_TRIPLE/$GCC_VERSION/cc1" ]] || \
    cross_port_fail "cc1 did not survive the prune"

# ---------------------------------------------------------------- libraries

# Copy the shared objects the kept binaries actually need, transitively. Doing
# this by closure rather than by a hand-written list is what keeps the prefix
# both complete and small; libc.so is excluded because the image has its own at
# /lib/libc.so, and it is the same musl version.
declare -A seen=()
queue=()
enqueue_needs() {
    local file="$1" need
    while read -r need; do
        [[ -n "$need" && "$need" != libc.so && -z "${seen[$need]:-}" ]] || continue
        seen[$need]=1
        queue+=("$need")
    done < <("$READELF" -d "$file" 2>/dev/null | \
        sed -n 's/.*Shared library: \[\([^]]*\)\].*/\1/p')
}

while read -r file; do
    "$READELF" -h "$file" >/dev/null 2>&1 || continue
    enqueue_needs "$file"
done < <(find "$GCC_ROOT$PREFIX/usr" -type f -perm -u+x)

while ((${#queue[@]})); do
    soname="${queue[0]}"
    queue=("${queue[@]:1}")
    src="$STAGE/usr/lib/$soname"
    [[ -e "$src" ]] || cross_port_fail "$soname is needed but Void did not install it"
    # Follow the SONAME symlink to the real file and recreate the link, so the
    # name the loader looks for is the name that is there.
    real=$(basename "$(readlink -f "$src")")
    cp -a "$(readlink -f "$src")" "$GCC_ROOT$PREFIX/usr/lib/$real"
    [[ "$real" == "$soname" ]] || ln -sfn "$real" "$GCC_ROOT$PREFIX/usr/lib/$soname"
    enqueue_needs "$GCC_ROOT$PREFIX/usr/lib/$real"
done

# Void builds gcc with the stack protector on by default, and its spec links
# -lssp_nonshared for the one symbol musl leaves to the compiler. It is a few
# hundred bytes and belongs next to the rest of the link line, in /usr/lib.
if [[ -f "$STAGE/usr/lib/libssp_nonshared.a" ]]; then
    cp -a "$STAGE/usr/lib/libssp_nonshared.a" "$GCC_ROOT/usr/lib/"
fi

# Void's own headers are deliberately not taken. The image's /usr/include is
# already a complete musl 1.2.6 sysroot with the Linux UAPI headers beside it,
# and it is the one tcc compiles against too -- two sets of libc headers on one
# machine is how you get a program that compiles and then does not run.

# ---------------------------------------------------------------- wrappers

# The prefix keeps Void's libraries away from the image's, so something has to
# put them back on the loader's path for these two programs only. cc1 and
# collect2 are started by the driver and inherit it.
for tool in gcc cpp; do
    cat > "$GCC_ROOT/usr/bin/$tool" <<EOF_WRAPPER
#!/bin/sh
# Generated by ports/build-gcc.sh.
LD_LIBRARY_PATH=$PREFIX/usr/lib\${LD_LIBRARY_PATH:+:\$LD_LIBRARY_PATH}
export LD_LIBRARY_PATH
exec $PREFIX/usr/bin/$tool "\$@"
EOF_WRAPPER
    chmod 0755 "$GCC_ROOT/usr/bin/$tool"
done

# Two linker names the image has never needed before, because nothing on it was
# ever linked *here*. A dynamic link resolves -lc against the shared musl, which
# lives in /lib, and -lgcc_s against the unwinder the graphics ports already
# ship; both are only reachable by their SONAME today. Static links needed
# neither: /usr/lib has libc.a and the crt objects.
ln -sfn ../../lib/libc.so "$GCC_ROOT/usr/lib/libc.so"
ln -sfn libgcc_s.so.1 "$GCC_ROOT/usr/lib/libgcc_s.so"

find "$GCC_ROOT" -name '*:*' -print -delete

# ---------------------------------------------------------------- verify

# A copy of what the image's /usr and /lib will hold, so the checks below
# compile against Tunix's musl rather than the build host's glibc.
rm -rf "$GCC_SYSROOT"
mkdir -p "$GCC_SYSROOT/usr/include" "$GCC_SYSROOT/usr/lib" "$GCC_SYSROOT/lib"
cp -a "$MUSL_SYSROOT/usr/include/." "$GCC_SYSROOT/usr/include/"
cp -a "$MUSL_SYSROOT/usr/lib/." "$GCC_SYSROOT/usr/lib/"
cp -a "$GCC_ROOT/usr/lib/." "$GCC_SYSROOT/usr/lib/"
# The image's unwinder, staged there by cross_port_stage_cxx_runtime.
cp -a "$CROSS_SYSROOT/lib/libgcc_s.so.1" "$GCC_SYSROOT/usr/lib/"
# In musl the loader *is* libc, and the image ships the real file under the
# loader's name with libc.so beside it as the link -- copy it the same way
# round, or /usr/lib/libc.so ends up pointing at a symlink loop and the linker
# quietly falls back to libc.a.
cp -a "$MUSL_SHARED_ROOT/lib/ld-musl-x86_64.so.1" "$GCC_SYSROOT/lib/"
ln -sfn ld-musl-x86_64.so.1 "$GCC_SYSROOT/lib/libc.so"

CHECK="$OUT/gcc-check"
rm -rf "$CHECK"
mkdir -p "$CHECK"

cat > "$CHECK/hello.c" <<'EOF_MAIN'
#include <stdio.h>
#include <linux/limits.h>

long long square(long long value);

int main(void) {
    /* Dividing a 128-bit value is a call into libgcc, and both operands come
       from another translation unit so it cannot be folded away: a missing
       libgcc.a fails the link instead of producing a binary that works anyway.
       Multiplying and dividing by the same number is its own expected value. */
    __int128 product = (__int128)square(31623) * 1000000009LL;
    long long recovered = (long long)(product / square(31623));
    printf("gcc works %lld %lld %d\n", square(7), recovered, PATH_MAX);
    return 0;
}
EOF_MAIN

cat > "$CHECK/square.c" <<'EOF_UNIT'
long long square(long long value) { return value * value; }
EOF_UNIT

run_gcc() {
    LD_LIBRARY_PATH="$GCC_ROOT$PREFIX/usr/lib" \
        "$GCC_ROOT$PREFIX/usr/bin/gcc" \
        --sysroot="$GCC_SYSROOT" -B "$BINUTILS_ROOT/usr/bin" "$@"
}

# sed rather than head: head closes the pipe after the first line, gcc dies of
# SIGPIPE, and pipefail turns that into a failed build.
version=$(run_gcc --version | sed -n 1p)
echo "build-gcc: $version"

# Static first: that is how everything else on the image is linked, and it needs
# only /usr/lib's crt objects and libc.a.
run_gcc -O2 -static -o "$CHECK/hello-static" "$CHECK/hello.c" "$CHECK/square.c"
if "$READELF" -l "$CHECK/hello-static" | grep -q 'program interpreter'; then
    cross_port_fail "the static test binary came out dynamic"
fi
output=$("$CHECK/hello-static")
[[ "$output" == "gcc works 49 1000000009 4096" ]] || \
    cross_port_fail "the static test binary printed '$output'"

# Then dynamic, which is the default and the one that has to find both the
# image's libc.so and its loader.
run_gcc -O2 -o "$CHECK/hello-dynamic" "$CHECK/hello.c" "$CHECK/square.c"
interp=$("$READELF" -l "$CHECK/hello-dynamic" | \
    sed -n 's/.*Requesting program interpreter: \([^]]*\).*/\1/p')
[[ "$interp" == "/lib/ld-musl-x86_64.so.1" ]] || \
    cross_port_fail "dynamic test binary asks for '${interp:-nothing}'"
output=$("$GCC_SYSROOT/lib/libc.so" --library-path "$GCC_SYSROOT/lib" "$CHECK/hello-dynamic")
[[ "$output" == "gcc works 49 1000000009 4096" ]] || \
    cross_port_fail "the dynamic test binary printed '$output'"

# Separate compilation through the image's own assembler and archiver, since
# that is what building anything real on the machine actually exercises.
run_gcc -O2 -c -o "$CHECK/square.o" "$CHECK/square.c"
"$BINUTILS_ROOT/usr/bin/ar" rcs "$CHECK/libsquare.a" "$CHECK/square.o"
run_gcc -O2 -static -o "$CHECK/hello-lib" "$CHECK/hello.c" -L"$CHECK" -lsquare
[[ "$("$CHECK/hello-lib")" == "gcc works 49 1000000009 4096" ]] || \
    cross_port_fail "linking against a static archive produced the wrong result"

# Nothing under the prefix may reach outside it for a library other than libc.
while read -r file; do
    while read -r need; do
        [[ -n "$need" && "$need" != libc.so ]] || continue
        [[ -e "$GCC_ROOT$PREFIX/usr/lib/$need" ]] || cross_port_fail \
            "$(basename "$file") needs $need, which is not in the prefix"
    done < <("$READELF" -d "$file" 2>/dev/null | \
        sed -n 's/.*Shared library: \[\([^]]*\)\].*/\1/p')
done < <(find "$GCC_ROOT$PREFIX" -type f -perm -u+x)

printf 'Tunix gcc root assembled at %s (%s, %s)\n' \
    "$GCC_ROOT" "$FULL_VERSION" "$(du -sh "$GCC_ROOT" | cut -f1)"
