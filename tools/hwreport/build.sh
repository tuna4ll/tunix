#!/bin/sh
set -eu

out=${1:?usage: build.sh IMAGE KERNEL LIMINE_DIR}
kernel=${2:?}
limine=${3:?}

work=$(mktemp -d "${TMPDIR:-/tmp}/tunix-hwreport.XXXXXX")
trap 'rm -rf "$work"' EXIT INT TERM

mkdir -p "$work/root/sbin" "$work/root/dev" "$work/root/proc" "$work/root/tmp"
${CC:-cc} -std=gnu11 -Wall -Wextra -Werror -O2 -static -nostdlib -nostartfiles \
	-ffreestanding -fno-stack-protector -fno-pic -fno-pie -no-pie -fno-builtin \
	-fno-asynchronous-unwind-tables tools/hwreport/init.c -o "$work/root/sbin/init"

TABLE=${TABLE:-gpt} ROOT_SLACK_MIB=64 tools/image.sh "$out" "$kernel" "$limine" \
	tools/hwreport/limine.conf "$work/root"
