#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)}
MUSL_SOURCE="$ROOT/ports/src/musl"
TCC_SOURCE="$ROOT/ports/src/tinycc"
OUT=${OUT:-$ROOT/ports/out}
SYSROOT="$OUT/sysroot"
MUSL_BUILD="$OUT/musl-build"
TCC_BUILD="$OUT/tcc-build"
TCC_ROOT="$OUT/tcc-root"
MUSL_CC="$SYSROOT/usr/bin/musl-gcc"
TOOLCHAIN_STAMP="$SYSROOT/.tunix-musl-toolchain-v4"
HOST_CC=${CC:-gcc}
HOST_AR=${AR:-ar}
HOST_RANLIB=${RANLIB:-ranlib}

fail() {
    echo "build-tcc: $*" >&2
    exit 1
}

[[ -x "$MUSL_SOURCE/configure" ]] || fail "missing musl source; initialize submodules"
[[ -x "$TCC_SOURCE/configure" ]] || fail "missing TinyCC source; run git submodule update --init --recursive"
command -v make >/dev/null 2>&1 || fail "make was not found"
command -v "$HOST_CC" >/dev/null 2>&1 || fail "$HOST_CC was not found"
command -v "$HOST_AR" >/dev/null 2>&1 || fail "$HOST_AR was not found"
command -v "$HOST_RANLIB" >/dev/null 2>&1 || fail "$HOST_RANLIB was not found"

NO_AUTO_ATOMIC=""
probe=$(mktemp)
trap 'rm -f "$probe" "$probe.bin"' EXIT
printf 'int main(void) { return 0; }\n' > "$probe"
if "$HOST_CC" -fno-link-libatomic -x c "$probe" -o "$probe.bin" >/dev/null 2>&1; then
    NO_AUTO_ATOMIC="-fno-link-libatomic"
fi
COMMON_CFLAGS="-O2 -fno-stack-protector -fno-pie $NO_AUTO_ATOMIC"
COMMON_LDFLAGS="-static -no-pie $NO_AUTO_ATOMIC"

if [[ ! -x "$MUSL_CC" || ! -f "$TOOLCHAIN_STAMP" ]]; then
    rm -rf "$MUSL_BUILD" "$SYSROOT"
    mkdir -p "$MUSL_BUILD" "$SYSROOT"
    (
        cd "$MUSL_BUILD"
        env CC="$HOST_CC" AR="$HOST_AR" RANLIB="$HOST_RANLIB" \
            CFLAGS="$COMMON_CFLAGS" LDFLAGS="$NO_AUTO_ATOMIC" \
            "$MUSL_SOURCE/configure" --prefix="$SYSROOT/usr" --disable-shared
        make -j"$JOBS"
        make install
    )
    : > "$TOOLCHAIN_STAMP"
fi

# TinyCC's c2str.exe bootstrap rule invokes $(CC) directly and does not append
# CFLAGS or LDFLAGS.  Keep GCC's automatic libatomic injection disabled and
# force non-PIE static executables at the wrapper level.  -fno-pie controls
# code generation, while -no-pie is required separately during the link step.
TCC_CC="$OUT/tcc-musl-gcc"
{
    printf '%s\n' '#!/bin/sh'
    printf '%s\n' 'set -eu'
    printf 'MUSL_CC=%q\n' "$MUSL_CC"
    printf 'NO_AUTO_ATOMIC=%q\n' "$NO_AUTO_ATOMIC"
    cat <<'WRAPPER'

link_mode=1
for arg in "$@"; do
    case "$arg" in
        -c|-S|-E|-M|-MM|-fsyntax-only)
            link_mode=0
            ;;
    esac
done

set -- "$@" -fno-pie
if [ -n "$NO_AUTO_ATOMIC" ]; then
    set -- "$@" "$NO_AUTO_ATOMIC"
fi
if [ "$link_mode" -eq 1 ]; then
    set -- "$@" -static -no-pie
fi

exec "$MUSL_CC" "$@"
WRAPPER
} > "$TCC_CC"
chmod 0755 "$TCC_CC"

rm -rf "$TCC_BUILD" "$TCC_ROOT"
mkdir -p "$TCC_BUILD" "$TCC_ROOT/usr/include" "$TCC_ROOT/usr/lib"

configure_args=(
    --source-path="$TCC_SOURCE"
    --prefix=/usr
    --bindir=/usr/bin
    --libdir=/usr/lib
    --tccdir=/usr/lib/tcc
    --includedir=/usr/include
    # Keep the installed compiler relocatable inside Tunix.  {B} expands to
    # TCC's runtime directory and {R} expands to its configured system root.
    --sysincludepaths={B}/include:{R}/usr/include
    --libpaths={R}/usr/lib:{B}
    --crtprefix={R}/usr/lib
    --enable-static
    --extra-cflags="-Os -fno-stack-protector -fno-pie $NO_AUTO_ATOMIC"
    --extra-ldflags="$COMMON_LDFLAGS"
)

configure_help=$($TCC_SOURCE/configure --help 2>&1 || true)
if grep -q -- 'musl' <<<"$configure_help"; then
    configure_args+=(--config-musl)
fi
if grep -q -- '--config-backtrace' <<<"$configure_help"; then
    configure_args+=(--config-backtrace=no)
fi
if grep -q -- '--config-bcheck' <<<"$configure_help"; then
    configure_args+=(--config-bcheck=no)
fi
if grep -q -- '--tcc-switches' <<<"$configure_help"; then
    configure_args+=(--tcc-switches=-static)
fi

(
    cd "$TCC_BUILD"
    env CC="$TCC_CC" AR="$HOST_AR" RANLIB="$HOST_RANLIB" \
        "$TCC_SOURCE/configure" "${configure_args[@]}"
    make -j"$JOBS" tcc
)

# libtcc1.a is built by the freshly-created TCC, not by the host compiler.
# Do not let that bootstrap compiler inspect /usr/include from WSL/Linux.
# The wrappers below expose only TinyCC's compiler headers and Tunix's musl
# sysroot while keeping the final installed search paths target-relative.
TCC_RUNTIME_CC="$TCC_BUILD/tcc-runtime-cc"
TCC_RUNTIME_AR="$TCC_BUILD/tcc-runtime-ar"
{
    printf '%s\n' '#!/bin/sh'
    printf '%s\n' 'set -eu'
    printf 'TCC_BIN=%q\n' "$TCC_BUILD/tcc"
    printf 'TCC_INTERNAL_INCLUDE=%q\n' "$TCC_SOURCE/include"
    printf 'TUNIX_INCLUDE=%q\n' "$SYSROOT/usr/include"
    cat <<'RUNTIME_CC'
exec "$TCC_BIN" \
    -nostdinc \
    -I"$TCC_INTERNAL_INCLUDE" \
    -I"$TUNIX_INCLUDE" \
    "$@"
RUNTIME_CC
} > "$TCC_RUNTIME_CC"
{
    printf '%s\n' '#!/bin/sh'
    printf '%s\n' 'set -eu'
    printf 'TCC_BIN=%q\n' "$TCC_BUILD/tcc"
    printf '%s\n' 'exec "$TCC_BIN" -ar "$@"'
} > "$TCC_RUNTIME_AR"
chmod 0755 "$TCC_RUNTIME_CC" "$TCC_RUNTIME_AR"

make -C "$TCC_BUILD/lib" -j"$JOBS" \
    XCC=../tcc-runtime-cc \
    XAR=../tcc-runtime-ar

cp -a "$SYSROOT/usr/include/." "$TCC_ROOT/usr/include/"
cp -a "$SYSROOT/usr/lib/." "$TCC_ROOT/usr/lib/"

make -C "$TCC_BUILD" install \
    bindir="$TCC_ROOT/usr/bin" \
    libdir="$TCC_ROOT/usr/lib" \
    tccdir="$TCC_ROOT/usr/lib/tcc" \
    includedir="$TCC_ROOT/usr/include" \
    mandir="$TCC_ROOT/usr/share/man" \
    infodir="$TCC_ROOT/usr/share/info" \
    docdir="$TCC_ROOT/usr/share/doc/tcc"

ln -sfn tcc "$TCC_ROOT/usr/bin/cc"

[[ -x "$TCC_ROOT/usr/bin/tcc" ]] || fail "staged compiler is missing"
[[ -f "$TCC_ROOT/usr/lib/tcc/libtcc1.a" ]] || fail "TinyCC runtime is missing"
[[ -f "$TCC_ROOT/usr/lib/libc.a" ]] || fail "musl libc.a is missing"
[[ -f "$TCC_ROOT/usr/lib/crt1.o" ]] || fail "musl crt1.o is missing"
[[ -f "$TCC_ROOT/usr/include/stdio.h" ]] || fail "musl headers are missing"

cp "$TCC_ROOT/usr/bin/tcc" "$OUT/tcc"
chmod 0755 "$OUT/tcc"

echo "TinyCC compiler built at $OUT/tcc"
echo "Tunix compiler root assembled at $TCC_ROOT"
