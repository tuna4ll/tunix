#!/bin/sh
set -eu

image=${1:?usage: read.sh IMAGE OUTPUT_DIR}
out=${2:?}

start=$(sfdisk -d "$image" | sed -n 's/.*start= *\([0-9]*\).*name="tunix-root".*/\1/p')
[ -n "$start" ] || { echo "read.sh: no tunix-root partition in $image" >&2; exit 1; }
filesystem="$image?offset=$((start * 512))"

mkdir -p "$out"
for name in $(debugfs -R 'ls -p /' "$filesystem" 2>/dev/null | awk -F/ '$6 ~ /^tunix-/ {print $6}'); do
	debugfs -R "dump /$name $out/$name" "$filesystem" 2>/dev/null
	echo "$out/$name"
done
