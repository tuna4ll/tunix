#!/usr/bin/env bash
# Assembles the rootfs for the void_distro experiment: Void Linux's own musl
# userland (bash, coreutils, its own musl loader, xbps -- whatever the base
# ROOTFS tarball ships) with just enough of Tunix laid on top to boot it:
# dinit as PID 1, the getty/chvt built for this kernel, and a login stand-in
# that skips authentication (see src/userspace/void-login.c for why).
#
# This is deliberately not the real image's build. That one assembles a
# rootfs from ~130 ports built against Tunix's own musl-cross toolchain and
# checks for hundreds of specific files; this one asks a single, narrower
# question -- does an *unmodified upstream* musl userland run on this kernel
# -- so it skips everything the real image needs and Void's tarball already
# answers for itself (coreutils, bash, its own C library).
#
# Usage: build-void-rootfs.sh OUTPUT_DIR TARBALL DINIT_ROOT GETTY CHVT VOID_LOGIN ZSTD_ROOT OVERLAY_DIR
set -euo pipefail

OUTPUT_DIR=$1
TARBALL=$2
DINIT_ROOT=$3
GETTY=$4
CHVT=$5
VOID_LOGIN=$6
ZSTD_ROOT=$7
OVERLAY_DIR=$8

rm -rf "$OUTPUT_DIR"
mkdir -p "$OUTPUT_DIR"

# Void's tarball is a full usr-merge layout (/bin, /sbin, /lib, /lib64 are all
# symlinks to usr/{bin,lib}), unlike the real image's rootfs which keeps /bin
# and /usr/bin as separate real directories. That merge is why every path
# added below targets usr/bin directly rather than bin/ -- a relative symlink
# built assuming split directories (../usr/bin/x) resolves to the wrong place
# once /sbin *is* usr/bin.
tar -xJf "$TARBALL" -C "$OUTPUT_DIR"

# dinit itself: static, so it does not care which libc the rest of the system
# uses. ports/build-dinit.sh's output ships sbin/{halt,poweroff,reboot,...}
# and usr/bin/dinit*, both meant for a rootfs where /bin and /usr/bin are
# separate real directories. Void's tree isn't one -- /sbin is a symlink to
# usr/bin -- so `cp -R dinit-root/. rootfs/` fails outright: cp refuses to
# merge a real "sbin" directory onto a destination where that name is already
# a symlink. Copying the two source directories' *contents* into usr/bin
# sidesteps the merge instead of needing one.
cp "$DINIT_ROOT"/sbin/* "$OUTPUT_DIR/usr/bin/"
cp "$DINIT_ROOT"/usr/bin/* "$OUTPUT_DIR/usr/bin/"
ln -sfn dinit "$OUTPUT_DIR/usr/bin/init"

# The service graph. type=scripted/process files, not binaries -- no libc
# question here.
mkdir -p "$OUTPUT_DIR/etc/dinit.d" "$OUTPUT_DIR/etc/rc.d"
cp -R "$OVERLAY_DIR/etc/dinit.d/." "$OUTPUT_DIR/etc/dinit.d/"
cp "$OVERLAY_DIR/etc/rc.d/rcS" "$OUTPUT_DIR/etc/rc.d/rcS"

# getty and chvt are the same statically linked binaries the real image
# ships, unrelated to Void's libc for the same reason dinit is. void-login
# overwrites Void's own /usr/bin/login -- see void-login.c for why this
# experiment does not attempt to make Tunix's PAM stack understand Void's
# /etc/shadow.
cp "$GETTY" "$OUTPUT_DIR/usr/bin/getty"
cp "$CHVT" "$OUTPUT_DIR/usr/bin/chvt"
cp "$VOID_LOGIN" "$OUTPUT_DIR/usr/bin/login"
# The base tarball has no standalone zstd(1) -- only libzstd.so.1, which
# libxbps links against directly -- so plain `tar` can't decompress a .xbps
# by hand (it shells out to zstd for that). This is the one binary in the
# whole image built with Tunix's own musl-cross toolchain rather than
# Void's: it only needs libc.so (readelf -d shows nothing else), so it runs
# against Void's own musl loader the same as everything else here does.
cp "$ZSTD_ROOT/usr/bin/zstd" "$OUTPUT_DIR/usr/bin/zstd"

chmod 0755 "$OUTPUT_DIR/etc/rc.d/rcS" \
	"$OUTPUT_DIR/usr/bin/getty" "$OUTPUT_DIR/usr/bin/chvt" \
	"$OUTPUT_DIR/usr/bin/login" \
	"$OUTPUT_DIR/usr/bin/zstd" \
	"$OUTPUT_DIR/usr/bin/dinit" \
	"$OUTPUT_DIR/usr/bin/dinitctl" "$OUTPUT_DIR/usr/bin/dinit-check" \
	"$OUTPUT_DIR/usr/bin/dinit-monitor"

echo "void_distro rootfs assembled at $OUTPUT_DIR"
