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

# BusyBox's DHCP client includes two Linux UAPI headers that are not
# shipped by musl itself.  Keep the port self-contained by installing the
# small ABI subset used by udhcpc into the Tunix sysroot.
mkdir -p "$SYSROOT/usr/include/linux"
cat > "$SYSROOT/usr/include/linux/if_packet.h" <<'EOF_IF_PACKET'
#ifndef TUNIX_LINUX_IF_PACKET_H
#define TUNIX_LINUX_IF_PACKET_H
#include <netpacket/packet.h>
#include <stdint.h>
struct tpacket_auxdata {
    uint32_t tp_status;
    uint32_t tp_len;
    uint32_t tp_snaplen;
    uint16_t tp_mac;
    uint16_t tp_net;
    uint16_t tp_vlan_tci;
    uint16_t tp_vlan_tpid;
};
#define TP_STATUS_CSUMNOTREADY (1U << 3)
#endif
EOF_IF_PACKET
cat > "$SYSROOT/usr/include/linux/filter.h" <<'EOF_FILTER'
#ifndef TUNIX_LINUX_FILTER_H
#define TUNIX_LINUX_FILTER_H
#include <stdint.h>
#define BPF_LD 0x00
#define BPF_LDX 0x01
#define BPF_ST 0x02
#define BPF_STX 0x03
#define BPF_ALU 0x04
#define BPF_JMP 0x05
#define BPF_RET 0x06
#define BPF_MISC 0x07
#define BPF_W 0x00
#define BPF_H 0x08
#define BPF_B 0x10
#define BPF_IMM 0x00
#define BPF_ABS 0x20
#define BPF_IND 0x40
#define BPF_MEM 0x60
#define BPF_LEN 0x80
#define BPF_MSH 0xa0
#define BPF_ADD 0x00
#define BPF_SUB 0x10
#define BPF_MUL 0x20
#define BPF_DIV 0x30
#define BPF_OR 0x40
#define BPF_AND 0x50
#define BPF_LSH 0x60
#define BPF_RSH 0x70
#define BPF_NEG 0x80
#define BPF_MOD 0x90
#define BPF_XOR 0xa0
#define BPF_JA 0x00
#define BPF_JEQ 0x10
#define BPF_JGT 0x20
#define BPF_JGE 0x30
#define BPF_JSET 0x40
#define BPF_K 0x00
#define BPF_X 0x08
struct sock_filter {
    uint16_t code;
    uint8_t jt;
    uint8_t jf;
    uint32_t k;
};
struct sock_fprog {
    unsigned short len;
    struct sock_filter *filter;
};
#define BPF_STMT(code_value, k_value) \
    { (unsigned short)(code_value), 0, 0, (k_value) }
#define BPF_JUMP(code_value, k_value, jt_value, jf_value) \
    { (unsigned short)(code_value), (jt_value), (jf_value), (k_value) }
#endif
EOF_FILTER

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
    RM RMDIR SED SEQ SHA256SUM SORT STAT TAIL TEE TEST TOUCH TR TRUE UNAME UNIQ WC WHICH XARGS YES HWCLOCK \
    IFCONFIG FEATURE_IFCONFIG_STATUS FEATURE_IFCONFIG_HW ROUTE ARP PING NSLOOKUP UDHCPC NETSTAT \
    NC FEATURE_NC_SERVER WGET FEATURE_WGET_STATUSBAR FEATURE_WGET_LONG_OPTIONS FEATURE_WGET_TIMEOUT
do
    set_config "$symbol" y
done

for symbol in \
    BUILD_LIBBUSYBOX FEATURE_SHARED_BUSYBOX FEATURE_PREFER_APPLETS SELINUX PAM NOMMU \
    ASH HUSH SH_IS_ASH SH_IS_HUSH INIT HALT POWEROFF REBOOT KBD_MODE LOADFONT LOADKMAP \
    OPENVT CHVT DEALLOCVT SETCONSOLE SETKEYCODES KLOGD LOGGER LOGREAD SYSLOGD FEATURE_KMSG_SYSLOG \
    MDEV MODPROBE INSMOD RMMOD LSMOD DEPMOD MOUNT UMOUNT SWAPON SWAPOFF DMESG FBSPLASH \
    FEATURE_IPV6 PING6 TRACEROUTE TRACEROUTE6 TELNET TELNETD IP FEATURE_UDHCPC_ARPING
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
