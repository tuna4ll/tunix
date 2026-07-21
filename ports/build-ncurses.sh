#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)}
NCURSES_SOURCE="$ROOT/ports/src/ncurses"
OUT=${OUT:-$ROOT/ports/out}
SYSROOT="$OUT/sysroot"
MUSL_CC="$SYSROOT/usr/bin/musl-gcc"
BUILD="$OUT/ncurses-build"
NCURSES_ROOT="$OUT/ncurses-root"
TERMINFO_SOURCE="$ROOT/ports/terminfo/tunix.ti"
HOST_AR=${AR:-ar}
HOST_RANLIB=${RANLIB:-ranlib}

fail() {
    echo "build-ncurses: $*" >&2
    exit 1
}

[[ -x "$NCURSES_SOURCE/configure" ]] || fail "missing ncurses source; initialize submodules"
[[ -f "$NCURSES_SOURCE/dist.mk" ]] || fail "ncurses dist.mk was not found"
ncurses_major=$(sed -n 's/^NCURSES_MAJOR[[:space:]]*=[[:space:]]*//p' "$NCURSES_SOURCE/dist.mk" | head -n1)
ncurses_minor=$(sed -n 's/^NCURSES_MINOR[[:space:]]*=[[:space:]]*//p' "$NCURSES_SOURCE/dist.mk" | head -n1)
[[ "$ncurses_major.$ncurses_minor" == "6.6" ]] || \
    fail "expected ncurses 6.6 source, found $ncurses_major.$ncurses_minor"
[[ -x "$MUSL_CC" ]] || fail "missing Tunix musl toolchain; build Bash first"
[[ -f "$NCURSES_SOURCE/misc/terminfo.src" ]] || fail "ncurses terminfo.src was not found"
[[ -f "$TERMINFO_SOURCE" ]] || fail "missing Tunix terminfo source"
command -v "$HOST_AR" >/dev/null 2>&1 || fail "ar was not found"
command -v "$HOST_RANLIB" >/dev/null 2>&1 || fail "ranlib was not found"

NO_AUTO_ATOMIC=""
probe=$(mktemp)
combined_terminfo=$(mktemp)
trap 'rm -f "$probe" "$probe.o" "$combined_terminfo"' EXIT
printf 'int tunix_probe;\n' > "$probe"
if "$MUSL_CC" -fno-link-libatomic -x c -c "$probe" -o "$probe.o" >/dev/null 2>&1; then
    NO_AUTO_ATOMIC="-fno-link-libatomic"
fi

rm -rf "$BUILD" "$NCURSES_ROOT"
mkdir -p "$BUILD" "$NCURSES_ROOT/usr/share/terminfo"

(
    cd "$BUILD"
    env \
        CC="$MUSL_CC" \
        AR="$HOST_AR" \
        RANLIB="$HOST_RANLIB" \
        CFLAGS="-Os -fno-stack-protector -fno-pie $NO_AUTO_ATOMIC" \
        CPPFLAGS="-D_GNU_SOURCE" \
        LDFLAGS="-static -no-pie $NO_AUTO_ATOMIC" \
        "$NCURSES_SOURCE/configure" \
            --prefix=/usr \
            --with-terminfo-dirs=/usr/share/terminfo \
            --with-default-terminfo-dir=/usr/share/terminfo \
            --enable-widec \
            --enable-pc-files \
            --with-pkg-config-libdir=/usr/lib/pkgconfig \
            --with-termlib \
            --with-normal \
            --without-shared \
            --without-debug \
            --without-ada \
            --without-cxx \
            --without-cxx-binding \
            --without-manpages \
            --without-tests \
            --disable-stripping
    make -j"$JOBS"
    make DESTDIR="$NCURSES_ROOT" install.libs install.includes install.progs
)

TIC="$BUILD/progs/tic"
[[ -x "$TIC" ]] || fail "the host-runnable tic utility was not built"
cat "$NCURSES_SOURCE/misc/terminfo.src" "$TERMINFO_SOURCE" > "$combined_terminfo"
# tunix* describe the kernel's own console. The xterm entries are for terminal
# emulators: weston-terminal sets TERM=xterm, as every graphical emulator does,
# and without the entry ncurses refuses to start at all -- nano and clear both
# exit with "unknown terminal type" inside a window that works perfectly.
# Entries reached through use= are inlined by tic, so only the names actually
# put in TERM need listing here.
"$TIC" -x -e 'tunix,tunix-256color,xterm,xterm-256color' \
    -o "$NCURSES_ROOT/usr/share/terminfo" "$combined_terminfo"

# The first-letter directory is named in hex on this build ("78" for x), so the
# entries are looked for by name rather than at a guessed path.
for entry in tunix xterm xterm-256color; do
    found=$(find "$NCURSES_ROOT/usr/share/terminfo" -type f -name "$entry" -print -quit)
    [[ -n "$found" ]] || fail "terminfo entry $entry was not compiled"
done

# Keep compatibility names expected by configure scripts and static linkers.
cd "$NCURSES_ROOT/usr/lib"
for library in ncursesw formw menuw panelw; do
    [[ -f "lib${library}.a" ]] || continue
    base=${library%w}
    ln -sfn "lib${library}.a" "lib${base}.a"
done
if [[ -f libtinfow.a ]]; then
    ln -sfn libtinfow.a libtinfo.a
fi

mkdir -p "$OUT"
printf '%s\n' 'ncurses static libraries and Tunix terminfo are ready.'
