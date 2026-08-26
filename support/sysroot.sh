#!/bin/bash
#
# Assemble the root filesystem.
#
# Tunix builds no userland of its own. Everything above the kernel is a Void
# Linux package, installed into a directory here by Void's own package manager,
# so what the machine runs is a stock glibc distribution rather than a hundred
# hand-written build scripts.
#
# Three things go in: Void's base ROOTFS tarball, the packages named by
# $VOID_INSTALL, and base-files/ from this repo laid over the result.
set -euo pipefail

SYSROOT=${1:?usage: sysroot.sh SYSROOT CACHE}
CACHE=${2:?usage: sysroot.sh SYSROOT CACHE}

MIRROR=${VOID_MIRROR:-https://repo-default.voidlinux.org}
ROOTFS_DATE=${VOID_ROOTFS_DATE:-20250202}
XBPS_STATIC_VERSION=${XBPS_STATIC_VERSION:-0.60.4_1}
INSTALL=${VOID_INSTALL:-base-files bash coreutils util-linux runit runit-void}
REMOVE=${VOID_REMOVE:-}

ROOTFS_TARBALL="$CACHE/void-x86_64-ROOTFS-$ROOTFS_DATE.tar.xz"
XBPS_TARBALL="$CACHE/xbps-static-$XBPS_STATIC_VERSION.tar.xz"
XBPS_DIR="$CACHE/xbps"

if [ "$(id -u)" != 0 ]; then
	echo "sysroot.sh: needs root -- the tarball carries ownership and device nodes" >&2
	exit 1
fi

# A directory that cannot record ownership or the setuid bit cannot hold a root
# filesystem: sudo and su would ship unprivileged, and every file in the image
# would belong to root. Windows drives mounted into WSL are the usual way to
# end up here, and the failure is silent -- chmod succeeds and changes nothing
# -- so it is worth one probe up front.
check_permissions() {
	local probe="$1/.permission-probe"
	mkdir -p "$1"
	rm -f "$probe"
	: > "$probe"
	# chown before chmod: changing the owner clears the setuid bit, so the
	# other order fails this check on a filesystem that is perfectly fine.
	chown 1:1 "$probe"
	chmod 4750 "$probe"
	local mode owner
	mode=$(stat -c %a "$probe")
	owner=$(stat -c %u:%g "$probe")
	rm -f "$probe"
	if [ "$mode" != 4750 ] || [ "$owner" != 1:1 ]; then
		echo "sysroot.sh: $1 does not keep permissions (got $mode $owner)." >&2
		echo "  Build the sysroot on a Linux filesystem instead:" >&2
		echo "    make image SYSROOT=/var/tmp/tunix-sysroot" >&2
		exit 1
	fi
}

fetch() {
	[ -f "$2" ] && return 0
	mkdir -p "$(dirname "$2")"
	echo ":: fetching $1"
	curl -fL --retry 3 -o "$2.part" "$1"
	mv "$2.part" "$2"
}

# --- the tools --------------------------------------------------------------
#
# xbps is taken as Void's own statically linked build rather than as a host
# package: it exists for every distribution this way, and it is the same
# version of the tool that made the repository it is about to read.
check_permissions "$(dirname "$SYSROOT")"

fetch "$MIRROR/static/xbps-static-static-$XBPS_STATIC_VERSION.x86_64-musl.tar.xz" \
	"$XBPS_TARBALL"
if [ ! -x "$XBPS_DIR/usr/bin/xbps-install" ]; then
	rm -rf "$XBPS_DIR"
	mkdir -p "$XBPS_DIR"
	tar -xJf "$XBPS_TARBALL" -C "$XBPS_DIR"
fi

fetch "$MIRROR/live/current/void-x86_64-ROOTFS-$ROOTFS_DATE.tar.xz" "$ROOTFS_TARBALL"

# --- the base ---------------------------------------------------------------

echo ":: unpacking the base rootfs"
rm -rf "$SYSROOT"
mkdir -p "$SYSROOT"
tar -xJpf "$ROOTFS_TARBALL" -C "$SYSROOT"

# The tarball's xbps.d points at whatever mirror it was built against; ours
# has to be the one the packages are actually coming from.
mkdir -p "$SYSROOT/etc/xbps.d"
printf 'repository=%s/current\n' "$MIRROR" > "$SYSROOT/etc/xbps.d/00-repository-main.conf"

# --- the packages -----------------------------------------------------------

export XBPS_ARCH=x86_64
XBPS="$XBPS_DIR/usr/bin"

# xbps first and on its own. The base tarball is cut a few times a year and the
# repository moves on without it; xbps refuses to install anything at all into a
# root whose own xbps package is older than the repository format, and says so
# with an error that does not mention the tarball.
echo ":: updating xbps"
"$XBPS/xbps-install" -S -y -u -r "$SYSROOT" xbps
echo ":: updating the base"
"$XBPS/xbps-install" -S -y -u -r "$SYSROOT"

echo ":: installing packages"
"$XBPS/xbps-install" -S -y -r "$SYSROOT" $INSTALL
if [ -n "$REMOVE" ]; then
	"$XBPS/xbps-remove" -R -y -r "$SYSROOT" $REMOVE
fi
"$XBPS/xbps-remove" -O -y -r "$SYSROOT"

# --- what makes it Tunix ----------------------------------------------------

echo ":: applying base-files"
cp -a base-files/overlay/. "$SYSROOT/"

# Appended rather than copied: Void's own packages own these files and add
# their system users to them, so replacing them would delete those.
for file in passwd group shadow; do
	[ -f "base-files/append/$file" ] || continue
	cat "base-files/append/$file" >> "$SYSROOT/etc/$file"
done

# One password for both accounts, and it is in the repository in plain sight:
# this is a machine you boot in an emulator to look at, not one anybody logs
# into over a network.
PASSWORD_HASH=$(sed -n 's/^tunix:\([^:]*\):.*/\1/p' base-files/append/shadow)
sed -i "s|^root:[^:]*:|root:$PASSWORD_HASH:|" "$SYSROOT/etc/shadow"
# Void puts wheel at gid 4, not the 10 it is on most distributions, so the
# gid is taken from the file rather than written into it.
sed -i 's|^wheel:x:\([0-9]*\):.*|wheel:x:\1:tunix|' "$SYSROOT/etc/group"

chown -R 1000:1000 "$SYSROOT/home/tunix"
chmod 0700 "$SYSROOT/home/tunix"
chmod 0755 "$SYSROOT/etc/rc.local"
chmod 0440 "$SYSROOT/etc/sudoers.d/tunix"

# /dev, /proc, /sys, /run and /tmp belong to the kernel, which fills them in at
# boot. Whatever a package left in them here would sit underneath and shadow
# the real thing -- an install script that redirected to /dev/null left a
# twenty-byte regular file there, and every `>/dev/null` in the system then
# failed with EACCES.
echo ":: clearing the pseudo-filesystem mount points"
for directory in dev proc sys run tmp; do
	rm -rf "${SYSROOT:?}/$directory"
	mkdir -p "$SYSROOT/$directory"
done
chmod 1777 "$SYSROOT/tmp"

# Cleared first, so the list in base-files/services is the whole answer rather
# than an addition to whatever the runit-void package happened to enable --
# which is six gettys and udevd.
echo ":: enabling services"
rm -rf "$SYSROOT/etc/runit/runsvdir/default"
mkdir -p "$SYSROOT/etc/runit/runsvdir/default"
while read -r service; do
	case "$service" in ''|'#'*) continue ;; esac
	if [ ! -d "$SYSROOT/etc/sv/$service" ]; then
		echo "sysroot.sh: no such service: $service" >&2
		exit 1
	fi
	ln -sfn "/etc/sv/$service" "$SYSROOT/etc/runit/runsvdir/default/$service"
done < base-files/services

if [ -f base-files/remove ]; then
	echo ":: trimming"
	while read -r path; do
		case "$path" in ''|'#'*) continue ;; esac
		rm -rf "${SYSROOT:?}/$path"
	done < base-files/remove
fi

echo ":: sysroot ready at $SYSROOT ($(du -sh "$SYSROOT" | cut -f1))"
