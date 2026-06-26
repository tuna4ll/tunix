#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)}
MUSL_SOURCE="$ROOT/ports/src/musl"
BASH_SOURCE_DIR="$ROOT/ports/src/bash"
OUT=${OUT:-$ROOT/ports/out}
SYSROOT="$OUT/sysroot"
MUSL_BUILD="$OUT/musl-build"
BASH_BUILD="$OUT/bash-build"
MUSL_CC="$SYSROOT/usr/bin/musl-gcc"
TOOLCHAIN_STAMP="$SYSROOT/.tunix-musl-toolchain-v4"
HOST_CC=${CC:-gcc}
HOST_AR=${AR:-ar}
HOST_RANLIB=${RANLIB:-ranlib}

fail() {
    echo "build-bash: $*" >&2
    exit 1
}

[[ -x "$MUSL_SOURCE/configure" ]] || fail "missing musl source"
[[ -x "$BASH_SOURCE_DIR/configure" ]] || fail "missing Bash source"
command -v "$HOST_AR" >/dev/null 2>&1 || fail "ar was not found"
command -v "$HOST_RANLIB" >/dev/null 2>&1 || fail "ranlib was not found"

NO_AUTO_ATOMIC=""
probe=$(mktemp)
trap 'rm -f "$probe" "$probe.o"' EXIT
printf 'int tunix_probe;\n' > "$probe"
if "$HOST_CC" -fno-link-libatomic -x c -c "$probe" -o "$probe.o" >/dev/null 2>&1; then
    NO_AUTO_ATOMIC="-fno-link-libatomic"
fi
COMMON_CFLAGS="-O2 -fno-stack-protector -fno-pie $NO_AUTO_ATOMIC"

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

rm -rf "$BASH_BUILD"
mkdir -p "$BASH_BUILD"
(
    cd "$BASH_BUILD"
    CC="$MUSL_CC" \
    CFLAGS="-Os -fno-stack-protector -fno-pie $NO_AUTO_ATOMIC" \
    LDFLAGS="-static -no-pie $NO_AUTO_ATOMIC" \
    "$BASH_SOURCE_DIR/configure" \
        --prefix=/ \
        --without-bash-malloc \
        --disable-nls \
        --disable-job-control
    make -j"$JOBS" bash
)

mkdir -p "$OUT"
cp "$BASH_BUILD/bash" "$OUT/bash"
chmod 0755 "$OUT/bash"
