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
[[ -x "$TCC_SOURCE/configure" ]] || fail "missing TinyCC source; run ports/init-tcc-submodule.sh"
command -v make >/dev/null 2>&1 || fail "make was not found"
command -v "$HOST_CC" >/dev/null 2>&1 || fail "$HOST_CC was not found"
command -v "$HOST_AR" >/dev/null 2>&1 || fail "$HOST_AR was not found"
command -v "$HOST_RANLIB" >/dev/null 2>&1 || fail "$HOST_RANLIB was not found"

NO_AUTO_ATOMIC=""
probe=$(mktemp)
trap 'rm -f "$probe" "$probe.o"' EXIT
printf 'int tunix_probe;\n' > "$probe"
if "$HOST_CC" -fno-link-libatomic -x c -c "$probe" -o "$probe.o" >/dev/null 2>&1; then
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

rm -rf "$TCC_BUILD"
mkdir -p "$TCC_BUILD"

configure_args=(
    --source-path="$TCC_SOURCE"
    --prefix=/usr
    --bindir=/usr/bin
    --libdir=/usr/lib
    --tccdir=/usr/lib/tcc
    --includedir=/usr/include
    --sysincludepaths=/usr/lib/tcc/include:/usr/include
    --libpaths=/usr/lib:/usr/lib/tcc
    --crtprefix=/usr/lib
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
    env CC="$MUSL_CC" AR="$HOST_AR" RANLIB="$HOST_RANLIB" \
        "$TCC_SOURCE/configure" "${configure_args[@]}"
    make -j"$JOBS" tcc
)

mkdir -p "$OUT"
cp "$TCC_BUILD/tcc" "$OUT/tcc"
chmod 0755 "$OUT/tcc"

file "$OUT/tcc" 2>/dev/null || true
echo "TinyCC compiler built at $OUT/tcc"
