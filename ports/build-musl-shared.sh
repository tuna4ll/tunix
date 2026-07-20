#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)}
MUSL_SOURCE="$ROOT/ports/src/musl"
OUT=${OUT:-$ROOT/ports/out}
BUILD="$OUT/musl-shared-build"
SYSROOT="$OUT/musl-shared-sysroot"
RUNTIME_ROOT="$OUT/musl-shared-root"
MUSL_CC="$SYSROOT/usr/bin/musl-gcc"
TOOLCHAIN_STAMP="$SYSROOT/.tunix-musl-shared-v1"
HOST_CC=${CC:-gcc}
HOST_AR=${AR:-ar}
HOST_RANLIB=${RANLIB:-ranlib}
HOST_STRIP=${STRIP:-strip}
READELF=${READELF:-readelf}

fail() {
    echo "build-musl-shared: $*" >&2
    exit 1
}

[[ -f "$MUSL_SOURCE/configure" ]] || fail "missing musl source"
chmod +x "$MUSL_SOURCE/configure" "$MUSL_SOURCE"/tools/*.sh
for tool in "$HOST_CC" "$HOST_AR" "$HOST_RANLIB" "$HOST_STRIP" "$READELF" python3 make; do
    command -v "$tool" >/dev/null 2>&1 || fail "$tool was not found"
done

NO_AUTO_ATOMIC=""
probe=$(mktemp)
trap 'rm -f "$probe" "$probe.o"' EXIT
printf 'int tunix_probe;\n' > "$probe"
if "$HOST_CC" -fno-link-libatomic -x c -c "$probe" -o "$probe.o" >/dev/null 2>&1; then
    NO_AUTO_ATOMIC="-fno-link-libatomic"
fi
COMMON_CFLAGS="-O2 -fno-stack-protector $NO_AUTO_ATOMIC"

if [[ ! -x "$MUSL_CC" || ! -f "$TOOLCHAIN_STAMP" ]]; then
    rm -rf "$BUILD" "$SYSROOT"
    mkdir -p "$BUILD" "$SYSROOT"
    (
        cd "$BUILD"
        env CC="$HOST_CC" AR="$HOST_AR" RANLIB="$HOST_RANLIB" \
            CFLAGS="$COMMON_CFLAGS" LDFLAGS="$NO_AUTO_ATOMIC" \
            bash "$MUSL_SOURCE/configure" \
                --prefix="$SYSROOT/usr" \
                --syslibdir="$SYSROOT/lib"
        make -j"$JOBS"
        make install
    )
    : > "$TOOLCHAIN_STAMP"
fi

rm -rf "$RUNTIME_ROOT"
mkdir -p "$RUNTIME_ROOT"
[[ -x "$MUSL_CC" ]] || fail "musl-gcc wrapper was not installed"
loader_host=$(find "$SYSROOT/lib" -maxdepth 1 -name 'ld-musl-*.so.1' -print -quit)
[[ -n "$loader_host" ]] || fail "dynamic loader was not installed"
loader_name=$(basename "$loader_host")
libc_host="$SYSROOT/usr/lib/libc.so"
[[ -f "$libc_host" ]] || fail "shared libc.so was not installed"

specs="$SYSROOT/usr/lib/musl-gcc.specs"
[[ -f "$specs" ]] || fail "musl-gcc specs were not installed"
python3 - "$specs" "$loader_name" <<'PY'
from pathlib import Path
import re
import sys
path = Path(sys.argv[1])
loader = "/lib/" + sys.argv[2]
text = path.read_text()
updated, count = re.subn(r"-dynamic-linker\s+\S+", "-dynamic-linker " + loader, text, count=1)
if count != 1:
    raise SystemExit("unable to rewrite musl dynamic-linker path")
path.write_text(updated)
PY

grep -q -- "-dynamic-linker /lib/$loader_name" "$specs" || \
    fail "musl-gcc still points at the build sysroot loader"

install -d "$RUNTIME_ROOT/lib" "$RUNTIME_ROOT/usr/bin" "$RUNTIME_ROOT/usr/lib" \
           "$RUNTIME_ROOT/etc"
cp -L "$loader_host" "$RUNTIME_ROOT/lib/$loader_name"
chmod 0755 "$RUNTIME_ROOT/lib/$loader_name"
ln -s "$loader_name" "$RUNTIME_ROOT/lib/libc.so"
printf '%s\n' '/lib:/usr/lib' > "$RUNTIME_ROOT/etc/ld-musl-x86_64.path"

SOURCE="$ROOT/tools/dynamic-runtime"
LIBRARY="$RUNTIME_ROOT/usr/lib/libtunix_dynamic.so.1"
"$MUSL_CC" -O2 -fPIC -shared -Wl,-soname,libtunix_dynamic.so.1 \
    "$SOURCE/libtunix-dynamic.c" -o "$LIBRARY"
ln -s libtunix_dynamic.so.1 "$RUNTIME_ROOT/usr/lib/libtunix_dynamic.so"

"$MUSL_CC" -O2 -fPIE -pie "$SOURCE/dynamic-hello.c" \
    -o "$RUNTIME_ROOT/usr/bin/dynamic-hello"
"$MUSL_CC" -O2 -fno-pie -no-pie "$SOURCE/dynamic-hello.c" \
    -o "$RUNTIME_ROOT/usr/bin/dynamic-nopie"
"$MUSL_CC" -O2 -fPIE -pie "$SOURCE/dlopen-test.c" -ldl \
    -o "$RUNTIME_ROOT/usr/bin/dlopen-test"
"$MUSL_CC" -O2 -fPIE -pie "$SOURCE/pthread-test.c" -pthread \
    -o "$RUNTIME_ROOT/usr/bin/pthread-test"
# The shared-memory primitives Wayland is built on: memfd, MAP_SHARED and
# passing the descriptor over SCM_RIGHTS.
"$MUSL_CC" -O2 -fPIE -pie "$SOURCE/shm-test.c" \
    -o "$RUNTIME_ROOT/usr/bin/shm-test"
# Signal delivery as a pollable descriptor, the way an event loop wants it.
"$MUSL_CC" -O2 -fPIE -pie "$SOURCE/signalfd-test.c" \
    -o "$RUNTIME_ROOT/usr/bin/signalfd-test"
# Killing a process that is asleep inside a rewound blocking syscall.
"$MUSL_CC" -O2 -fPIE -pie "$SOURCE/kill-blocked-test.c" \
    -o "$RUNTIME_ROOT/usr/bin/kill-blocked-test"

for binary in dynamic-hello dynamic-nopie dlopen-test pthread-test shm-test signalfd-test kill-blocked-test; do
    interp=$($READELF -l "$RUNTIME_ROOT/usr/bin/$binary" | \
        sed -n 's/.*Requesting program interpreter: \([^]]*\).*/\1/p')
    [[ "$interp" == "/lib/$loader_name" ]] || \
        fail "$binary has unexpected interpreter '${interp:-missing}'"
done

hello_type=$($READELF -h "$RUNTIME_ROOT/usr/bin/dynamic-hello" | \
    sed -n 's/^[[:space:]]*Type:[[:space:]]*\([^[:space:]]*\).*/\1/p')
nopie_type=$($READELF -h "$RUNTIME_ROOT/usr/bin/dynamic-nopie" | \
    sed -n 's/^[[:space:]]*Type:[[:space:]]*\([^[:space:]]*\).*/\1/p')
[[ "$hello_type" == DYN ]] || fail "dynamic-hello is not ET_DYN"
[[ "$nopie_type" == EXEC ]] || fail "dynamic-nopie is not ET_EXEC"

host_loader="$RUNTIME_ROOT/lib/$loader_name"
library_path="$RUNTIME_ROOT/lib:$SYSROOT/usr/lib:$RUNTIME_ROOT/usr/lib"
"$host_loader" --library-path "$library_path" \
    "$RUNTIME_ROOT/usr/bin/dynamic-hello" host-check
"$host_loader" --library-path "$library_path" \
    "$RUNTIME_ROOT/usr/bin/dynamic-nopie" host-check
"$host_loader" --library-path "$library_path" \
    "$RUNTIME_ROOT/usr/bin/dlopen-test" "$LIBRARY"
"$host_loader" --library-path "$library_path" \
    "$RUNTIME_ROOT/usr/bin/pthread-test"
"$host_loader" --library-path "$library_path" \
    "$RUNTIME_ROOT/usr/bin/shm-test"
"$host_loader" --library-path "$library_path" \
    "$RUNTIME_ROOT/usr/bin/signalfd-test"
"$host_loader" --library-path "$library_path" \
    "$RUNTIME_ROOT/usr/bin/kill-blocked-test"

"$HOST_STRIP" --strip-unneeded "$LIBRARY"
for binary in dynamic-hello dynamic-nopie dlopen-test pthread-test shm-test signalfd-test kill-blocked-test; do
    "$HOST_STRIP" --strip-all "$RUNTIME_ROOT/usr/bin/$binary"
done

cat > "$RUNTIME_ROOT/usr/bin/dynamic-runtime-check" <<'EOF_CHECK'
#!/bin/sh
set -eu
/usr/bin/dynamic-hello tunix-check
/usr/bin/dynamic-nopie tunix-check
/usr/bin/dlopen-test
/usr/bin/pthread-test
/usr/bin/shm-test
/usr/bin/signalfd-test
/usr/bin/kill-blocked-test
printf '%s\n' 'dynamic-runtime-check: PASS'
EOF_CHECK
chmod 0755 "$RUNTIME_ROOT/usr/bin/dynamic-runtime-check"

printf 'Shared musl runtime ready: %s\n' "$loader_name"
