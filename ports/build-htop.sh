#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)}
HTOP_SOURCE="$ROOT/ports/src/htop"
OUT=${OUT:-$ROOT/ports/out}
SYSROOT="$OUT/sysroot"
MUSL_CC="$SYSROOT/usr/bin/musl-gcc"
NCURSES_ROOT="$OUT/ncurses-root"
SOURCE_WORK="$OUT/htop-source"
BUILD="$OUT/htop-build"
HTOP_BINARY="$OUT/htop"
HOST_AR=${AR:-ar}
HOST_RANLIB=${RANLIB:-ranlib}

fail() {
    echo "build-htop: $*" >&2
    exit 1
}

[[ -f "$HTOP_SOURCE/htop.c" ]] || fail "missing htop source; initialize submodules"
[[ -x "$MUSL_CC" ]] || fail "missing Tunix musl toolchain"
[[ -f "$NCURSES_ROOT/usr/lib/libncursesw.a" ]] || fail "ncursesw was not built"
[[ -d "$NCURSES_ROOT/usr/share/terminfo" ]] || fail "Tunix terminfo database was not built"
command -v pkg-config >/dev/null 2>&1 || fail "pkg-config is required by the htop configure script"
command -v "$HOST_AR" >/dev/null 2>&1 || fail "ar was not found"
command -v "$HOST_RANLIB" >/dev/null 2>&1 || fail "ranlib was not found"

NO_AUTO_ATOMIC=""
probe=$(mktemp)
trap 'rm -f "$probe" "$probe.o"' EXIT
printf 'int tunix_probe;\n' > "$probe"
if "$MUSL_CC" -fno-link-libatomic -x c -c "$probe" -o "$probe.o" >/dev/null 2>&1; then
    NO_AUTO_ATOMIC="-fno-link-libatomic"
fi

rm -rf "$SOURCE_WORK" "$BUILD"
mkdir -p "$SOURCE_WORK" "$BUILD"
cp -a "$HTOP_SOURCE/." "$SOURCE_WORK/"

if [[ ! -x "$SOURCE_WORK/configure" ]]; then
    command -v autoreconf >/dev/null 2>&1 || fail "autoreconf is required for the htop Git checkout"
    (
        cd "$SOURCE_WORK"
        ./autogen.sh || fail "htop autotools bootstrap failed; install autoconf, automake and pkg-config"
    )
fi
[[ -x "$SOURCE_WORK/configure" ]] || fail "htop configure script was not generated"

# htop links ncursesw directly out of the Tunix ncurses staging tree; the
# CURSES_* variables suppress the pkg-config probe entirely, which would
# otherwise pick up the build host's ncurses.
CURSES_CFLAGS="-I$NCURSES_ROOT/usr/include"
CURSES_LIBS="-L$NCURSES_ROOT/usr/lib -lncursesw"
if [[ -f "$NCURSES_ROOT/usr/lib/libtinfow.a" ]]; then
    CURSES_LIBS="$CURSES_LIBS -ltinfow"
fi

# Every optional dependency below wants a shared library or a Linux kernel
# interface that Tunix does not provide (libsensors, libcap, hwloc, libnl,
# taskstats delayacct) or glibc-only backtrace support.
configure_args=(
    --prefix=/usr
    --sysconfdir=/etc
    --host=x86_64-linux-musl
    --enable-unicode
    --disable-affinity
    --disable-backtrace
    --disable-capabilities
    --disable-delayacct
    --disable-hwloc
    --disable-sensors
    --disable-shared
    --without-libdl
)
configure_help=$("$SOURCE_WORK/configure" --help 2>&1 || true)
supported_args=()
for option in "${configure_args[@]}"; do
    case "$option" in
        --prefix=*|--sysconfdir=*|--host=*)
            supported_args+=("$option")
            ;;
        *)
            # configure --help advertises toggles in their --enable-/--with-
            # spelling only, so probe for that form before passing the
            # --disable-/--without- variant we actually want.
            probe_option=${option%%=*}
            probe_option=${probe_option/#--disable-/--enable-}
            probe_option=${probe_option/#--without-/--with-}
            if grep -Fq -- "$probe_option" <<<"$configure_help"; then
                supported_args+=("$option")
            fi
            ;;
    esac
done

(
    cd "$BUILD"
    env \
        CC="$MUSL_CC" \
        AR="$HOST_AR" \
        RANLIB="$HOST_RANLIB" \
        CURSES_CFLAGS="$CURSES_CFLAGS" \
        CURSES_LIBS="$CURSES_LIBS" \
        PKG_CONFIG_PATH="$NCURSES_ROOT/usr/lib/pkgconfig" \
        PKG_CONFIG_LIBDIR="$NCURSES_ROOT/usr/lib/pkgconfig" \
        CFLAGS="-Os -fno-stack-protector -fno-pie $NO_AUTO_ATOMIC" \
        CPPFLAGS="-D_GNU_SOURCE" \
        LDFLAGS="-static -no-pie $NO_AUTO_ATOMIC" \
        "$SOURCE_WORK/configure" "${supported_args[@]}"
    make -j"$JOBS"
)

[[ -x "$BUILD/htop" ]] || fail "htop binary was not produced"
cp "$BUILD/htop" "$HTOP_BINARY"
chmod 0755 "$HTOP_BINARY"

if command -v readelf >/dev/null 2>&1; then
    if readelf -l "$HTOP_BINARY" | grep -q 'INTERP'; then
        fail "htop unexpectedly contains a dynamic interpreter"
    fi
    if readelf -d "$HTOP_BINARY" 2>/dev/null | grep -q 'NEEDED'; then
        fail "htop unexpectedly contains dynamic dependencies"
    fi
fi

htop_version=$(TERM=tunix-256color \
TERMINFO="$NCURSES_ROOT/usr/share/terminfo" \
    "$HTOP_BINARY" --version 2>&1 | sed -n '1s/^htop //p')
[[ -n "$htop_version" ]] || fail "built htop did not report a version"

printf '%s\n' "htop $htop_version static binary is ready."
