#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)}
MUSL_SOURCE="$ROOT/ports/src/musl"
BUSYBOX_SOURCE="$ROOT/ports/src/busybox"
OUT=${OUT:-$ROOT/ports/out}
SYSROOT="$OUT/sysroot"
MUSL_BUILD="$OUT/musl-build"
BUILD="$OUT/busybox-build"
MUSL_CC="$SYSROOT/usr/bin/musl-gcc"
TOOLCHAIN_STAMP="$SYSROOT/.tunix-musl-toolchain-v4"
HOST_CC=${CC:-gcc}
HOST_AR=${AR:-ar}
HOST_RANLIB=${RANLIB:-ranlib}

fail() {
    echo "build-busybox: $*" >&2
    exit 1
}

[[ -f "$BUSYBOX_SOURCE/Makefile" ]] || fail "missing BusyBox source"
[[ -x "$MUSL_SOURCE/configure" ]] || fail "missing musl source"
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

rm -rf "$BUILD"
mkdir -p "$BUILD"
make -C "$BUSYBOX_SOURCE" O="$BUILD" allnoconfig >/dev/null
CONFIG_FILE="$BUILD/.config"
[[ -f "$CONFIG_FILE" ]] || fail "BusyBox configuration was not created"

set_config() {
    local symbol="$1"
    local value="$2"
    if [[ "$value" == y ]]; then
        sed -i -e "s|^# CONFIG_${symbol} is not set$|CONFIG_${symbol}=y|" \
               -e "s|^CONFIG_${symbol}=.*$|CONFIG_${symbol}=y|" "$CONFIG_FILE"
        grep -q "^CONFIG_${symbol}=y$" "$CONFIG_FILE" || printf 'CONFIG_%s=y\n' "$symbol" >> "$CONFIG_FILE"
    else
        sed -i -e "s|^CONFIG_${symbol}=.*$|# CONFIG_${symbol} is not set|" "$CONFIG_FILE"
        grep -q "^# CONFIG_${symbol} is not set$" "$CONFIG_FILE" || printf '# CONFIG_%s is not set\n' "$symbol" >> "$CONFIG_FILE"
    fi
}

for symbol in \
    STATIC BUSYBOX AWK BASENAME CAT CHMOD CLEAR CP CUT DATE DD DIRNAME DU ECHO ENV EXPR FALSE \
    FIND GREP EGREP FGREP HEAD ID LS MD5SUM MKDIR MV PRINTENV PRINTF PWD READLINK REALPATH \
    RM RMDIR SED SEQ SHA256SUM SORT STAT TAIL TEE TEST TOUCH TR TRUE UNAME UNIQ WC WHICH XARGS YES HWCLOCK
do
    set_config "$symbol" y
done

for symbol in \
    BUILD_LIBBUSYBOX FEATURE_SHARED_BUSYBOX FEATURE_PREFER_APPLETS SELINUX PAM NOMMU \
    ASH HUSH SH_IS_ASH SH_IS_HUSH INIT HALT POWEROFF REBOOT KBD_MODE LOADFONT LOADKMAP \
    OPENVT CHVT DEALLOCVT SETCONSOLE SETKEYCODES KLOGD LOGGER LOGREAD SYSLOGD FEATURE_KMSG_SYSLOG \
    MDEV MODPROBE INSMOD RMMOD LSMOD DEPMOD MOUNT UMOUNT SWAPON SWAPOFF DMESG FBSPLASH
do
    set_config "$symbol" n
done

if grep -q '^CONFIG_EXTRA_LDLIBS=' "$CONFIG_FILE"; then
    sed -i 's|^CONFIG_EXTRA_LDLIBS=.*|CONFIG_EXTRA_LDLIBS=""|' "$CONFIG_FILE"
else
    printf '%s\n' 'CONFIG_EXTRA_LDLIBS=""' >> "$CONFIG_FILE"
fi

set +o pipefail
yes "" | make -C "$BUSYBOX_SOURCE" O="$BUILD" oldconfig >/dev/null
status=${PIPESTATUS[1]}
set -o pipefail
[[ "$status" -eq 0 ]] || fail "BusyBox oldconfig failed"

make -C "$BUSYBOX_SOURCE" O="$BUILD" \
    CC="$MUSL_CC" HOSTCC="${HOSTCC:-gcc}" AR="$HOST_AR" RANLIB="$HOST_RANLIB" \
    CFLAGS="-Os -fno-stack-protector -fno-pie $NO_AUTO_ATOMIC" \
    LDFLAGS="$COMMON_LDFLAGS" -j"$JOBS"

mkdir -p "$OUT"
cp "$BUILD/busybox" "$OUT/busybox"
chmod 0755 "$OUT/busybox"
