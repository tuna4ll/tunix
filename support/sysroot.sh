#!/bin/bash
set -euo pipefail

SYSROOT=${1:?usage: sysroot.sh SYSROOT CACHE}
CACHE=${2:?usage: sysroot.sh SYSROOT CACHE}

MIRROR=${VOID_MIRROR:-https://repo-default.voidlinux.org}
ROOTFS_DATE=${VOID_ROOTFS_DATE:-20250202}
ARCH=${VOID_ARCH:-x86_64}
XBPS_STATIC_VERSION=${XBPS_STATIC_VERSION:-0.60.4_1}
INSTALL=${VOID_INSTALL:-base-files bash coreutils util-linux runit runit-void}
REMOVE=${VOID_REMOVE:-}

ROOTFS_TARBALL="$CACHE/void-$ARCH-ROOTFS-$ROOTFS_DATE.tar.xz"
XBPS_TARBALL="$CACHE/xbps-static-$XBPS_STATIC_VERSION.tar.xz"
XBPS_DIR="$CACHE/xbps"

if [ "$(id -u)" != 0 ]; then
	echo "sysroot.sh: needs root -- the tarball carries ownership and device nodes" >&2
	exit 1
fi

check_permissions() {
	local probe="$1/.permission-probe"
	mkdir -p "$1"
	rm -f "$probe"
	: > "$probe"
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

check_permissions "$(dirname "$SYSROOT")"

fetch "$MIRROR/static/xbps-static-static-$XBPS_STATIC_VERSION.x86_64-musl.tar.xz" \
	"$XBPS_TARBALL"
if [ ! -x "$XBPS_DIR/usr/bin/xbps-install" ]; then
	rm -rf "$XBPS_DIR"
	mkdir -p "$XBPS_DIR"
	tar -xJf "$XBPS_TARBALL" -C "$XBPS_DIR"
fi

fetch "$MIRROR/live/current/void-$ARCH-ROOTFS-$ROOTFS_DATE.tar.xz" "$ROOTFS_TARBALL"

echo ":: unpacking the base rootfs"
rm -rf "$SYSROOT"
mkdir -p "$SYSROOT"
tar -xJpf "$ROOTFS_TARBALL" -C "$SYSROOT"

mkdir -p "$SYSROOT/etc/xbps.d"
REPOSITORY="$MIRROR/current"
[ "$ARCH" = x86_64 ] || REPOSITORY="$MIRROR/current/$ARCH"
printf 'repository=%s\n' "$REPOSITORY" > "$SYSROOT/etc/xbps.d/00-repository-main.conf"

export XBPS_ARCH=$ARCH
XBPS="$XBPS_DIR/usr/bin"

mkdir -p "$CACHE/packages"
PACKAGES=$(cd "$CACHE/packages" && pwd)
xbps_install() {
	"$XBPS/xbps-install" -c "$PACKAGES" -r "$SYSROOT" "$@"
}

echo ":: updating xbps"
xbps_install -S -y -u xbps
echo ":: updating the base"
xbps_install -S -y -u

echo ":: installing packages"
xbps_install -S -y $INSTALL
if [ -n "$REMOVE" ]; then
	"$XBPS/xbps-remove" -R -y -r "$SYSROOT" $REMOVE
fi
"$XBPS/xbps-remove" -O -y -r "$SYSROOT" || true

echo ":: applying base-files"
cp -a base-files/overlay/. "$SYSROOT/"

for file in passwd group shadow; do
	[ -f "base-files/append/$file" ] || continue
	cat "base-files/append/$file" >> "$SYSROOT/etc/$file"
done

PASSWORD_HASH=$(sed -n 's/^tunix:\([^:]*\):.*/\1/p' base-files/append/shadow)
sed -i "s|^root:[^:]*:|root:$PASSWORD_HASH:|" "$SYSROOT/etc/shadow"
sed -i 's|^\(root:.*\):/bin/sh$|\1:/bin/bash|' "$SYSROOT/etc/passwd"
sed -i 's|^wheel:x:\([0-9]*\):.*|wheel:x:\1:tunix|' "$SYSROOT/etc/group"
sed -i 's|^_seatd:x:\([0-9]*\):.*|_seatd:x:\1:tunix|' "$SYSROOT/etc/group"
sed -i 's|^audio:x:\([0-9]*\):.*|audio:x:\1:tunix|' "$SYSROOT/etc/group"

chown -R 1000:1000 "$SYSROOT/home/tunix"
chmod 0700 "$SYSROOT/home/tunix"
chmod 0755 "$SYSROOT/etc/rc.local"
for service in base-files/overlay/etc/sv/*/run base-files/overlay/etc/sv/*/log/run; do
	[ -f "$service" ] || continue
	chmod 0755 "$SYSROOT${service#base-files/overlay}"
done
chmod 0750 "$SYSROOT/etc/sudoers.d"
chmod 0440 "$SYSROOT/etc/sudoers.d/tunix"
chown 0:0 "$SYSROOT/etc/sudoers.d" "$SYSROOT/etc/sudoers.d/tunix"

echo ":: clearing the pseudo-filesystem mount points"
for directory in dev proc sys run tmp; do
	rm -rf "${SYSROOT:?}/$directory"
	mkdir -p "$SYSROOT/$directory"
done
chmod 1777 "$SYSROOT/tmp"

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
done < <(cat base-files/services "base-files/services-${DESKTOP:-gnome}" 2>/dev/null)

if [ -f base-files/remove ]; then
	echo ":: trimming"
	while read -r path; do
		case "$path" in ''|'#'*) continue ;; esac
		rm -rf "${SYSROOT:?}/$path"
	done < base-files/remove
fi

echo ":: sysroot ready at $SYSROOT ($(du -sh "$SYSROOT" | cut -f1))"
