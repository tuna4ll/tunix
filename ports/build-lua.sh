#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
LUA_SOURCE="$ROOT/ports/src/lua"
OUT=${OUT:-$ROOT/ports/out}
SYSROOT="$OUT/sysroot"
MUSL_CC="$SYSROOT/usr/bin/musl-gcc"
BUILD="$OUT/lua-build"
OBJECTS="$BUILD/obj"
LUA_ROOT="$OUT/lua-root"
HOST_AR=${AR:-ar}
HOST_RANLIB=${RANLIB:-ranlib}
HOST_STRIP=${STRIP:-strip}

fail() {
    echo "build-lua: $*" >&2
    exit 1
}

[[ -f "$LUA_SOURCE/lua.c" ]] || \
    fail "missing Lua source; run scripts/add-lua-submodule.sh or initialize submodules"
[[ -f "$LUA_SOURCE/lua.h" ]] || fail "Lua public header was not found"
[[ -x "$MUSL_CC" ]] || fail "missing Tunix musl toolchain; build Bash first"
command -v "$HOST_AR" >/dev/null 2>&1 || fail "$HOST_AR was not found"
command -v "$HOST_RANLIB" >/dev/null 2>&1 || fail "$HOST_RANLIB was not found"
command -v "$HOST_STRIP" >/dev/null 2>&1 || fail "$HOST_STRIP was not found"
command -v install >/dev/null 2>&1 || fail "install was not found"

major=$(awk '$2 == "LUA_VERSION_MAJOR_N" { print $3; exit }' "$LUA_SOURCE/lua.h")
minor=$(awk '$2 == "LUA_VERSION_MINOR_N" { print $3; exit }' "$LUA_SOURCE/lua.h")
release=$(awk '$2 == "LUA_VERSION_RELEASE_N" { print $3; exit }' "$LUA_SOURCE/lua.h")
[[ "$major.$minor.$release" == "5.5.0" ]] || \
    fail "expected Lua 5.5.0, found ${major:-?}.${minor:-?}.${release:-?}"

CFLAGS=(
    -Os
    -std=c99
    -Wall
    -fno-stack-protector
    -fno-pie
    -fno-common
    -ffunction-sections
    -fdata-sections
    -DLUA_USE_POSIX
)
LDFLAGS=(
    -static
    -no-pie
    -Wl,--gc-sections
)

probe=$(mktemp)
trap 'rm -f "$probe" "$probe.o"' EXIT
printf 'int tunix_probe;\n' > "$probe"
if "$MUSL_CC" -fno-link-libatomic -x c -c "$probe" -o "$probe.o" >/dev/null 2>&1; then
    CFLAGS+=(-fno-link-libatomic)
    LDFLAGS+=(-fno-link-libatomic)
fi

rm -rf "$BUILD" "$LUA_ROOT"
mkdir -p "$BUILD" "$OBJECTS" "$LUA_ROOT/usr/bin" "$LUA_ROOT/usr/lib" \
    "$LUA_ROOT/usr/include/lua5.5" "$LUA_ROOT/usr/lib/pkgconfig" \
    "$LUA_ROOT/usr/share/lua/5.5"

# Build from a disposable copy so the exact submodule checkout remains clean.
find "$LUA_SOURCE" -maxdepth 1 -type f -exec cp -p {} "$BUILD/" \;

# Configure Lua's install paths in the disposable build copy.
# LUA_ROOT is defined unconditionally by upstream luaconf.h, so a -D flag
# would only trigger a redefinition warning and would not reliably win.
sed -E -i \
    's|^[[:space:]]*#define[[:space:]]+LUA_ROOT[[:space:]]+"/usr/local/"[[:space:]]*$|#define LUA_ROOT "/usr/"|' \
    "$BUILD/luaconf.h"
grep -Eq '^[[:space:]]*#define[[:space:]]+LUA_ROOT[[:space:]]+"/usr/"([[:space:]]|$)' \
    "$BUILD/luaconf.h" || \
    fail "failed to configure Lua module paths for /usr"

core_sources=(
    lapi.c lcode.c lctype.c ldebug.c ldo.c ldump.c lfunc.c lgc.c
    llex.c lmem.c lobject.c lopcodes.c lparser.c lstate.c lstring.c
    ltable.c ltm.c lundump.c lvm.c lzio.c
)
library_sources=(
    lauxlib.c lbaselib.c ldblib.c liolib.c lmathlib.c loslib.c
    ltablib.c lstrlib.c lutf8lib.c loadlib.c lcorolib.c linit.c
)
all_sources=("${core_sources[@]}" "${library_sources[@]}")
library_objects=()

for source in "${all_sources[@]}"; do
    [[ -f "$BUILD/$source" ]] || fail "Lua source file $source is missing"
    object="$OBJECTS/${source%.c}.o"
    "$MUSL_CC" "${CFLAGS[@]}" -I"$BUILD" -c "$BUILD/$source" -o "$object"
    library_objects+=("$object")
done

rm -f "$BUILD/liblua.a"
"$HOST_AR" rc "$BUILD/liblua.a" "${library_objects[@]}"
"$HOST_RANLIB" "$BUILD/liblua.a"

"$MUSL_CC" "${CFLAGS[@]}" -I"$BUILD" -c "$BUILD/lua.c" \
    -o "$OBJECTS/lua.o"
"$MUSL_CC" "${LDFLAGS[@]}" -o "$BUILD/lua" \
    "$OBJECTS/lua.o" "$BUILD/liblua.a" -lm

[[ -x "$BUILD/lua" ]] || fail "Lua interpreter was not produced"
[[ -f "$BUILD/liblua.a" ]] || fail "Lua static library was not produced"

"$HOST_STRIP" --strip-all "$BUILD/lua"
install -m 0755 "$BUILD/lua" "$LUA_ROOT/usr/bin/lua"
install -m 0644 "$BUILD/liblua.a" "$LUA_ROOT/usr/lib/liblua.a"
for header in lua.h lauxlib.h lualib.h; do
    install -m 0644 "$LUA_SOURCE/$header" "$LUA_ROOT/usr/include/lua5.5/$header"
done
install -m 0644 "$BUILD/luaconf.h" "$LUA_ROOT/usr/include/lua5.5/luaconf.h"

# lua/lua's Git repository does not ship the convenience C++ wrapper that is
# present in Lua release tarballs. Generate the canonical wrapper locally
# instead of treating it as an upstream source file.
cat > "$LUA_ROOT/usr/include/lua5.5/lua.hpp" <<'EOF_HPP'
#ifndef LUA_HPP
#define LUA_HPP

extern "C" {
#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"
}

#endif /* LUA_HPP */
EOF_HPP
ln -sfn lua "$LUA_ROOT/usr/bin/lua5.5"

cat > "$LUA_ROOT/usr/lib/pkgconfig/lua.pc" <<'EOF_PC'
prefix=/usr
exec_prefix=${prefix}
libdir=${exec_prefix}/lib
includedir=${prefix}/include/lua5.5

Name: Lua
Description: Lua language engine
Version: 5.5.0
Libs: -L${libdir} -llua -lm
Cflags: -I${includedir}
EOF_PC

# Keep convenient output paths consistent with the other ports.
cp "$LUA_ROOT/usr/bin/lua" "$OUT/lua"
chmod 0755 "$OUT/lua"

OUT="$OUT" "$ROOT/scripts/test-lua-port.sh"
printf '%s\n' "Lua 5.5.0 staged at $LUA_ROOT"
