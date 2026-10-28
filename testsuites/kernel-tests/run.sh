#!/bin/sh
set -eu

. testsuites/boot.sh

tests=${*:-process memory swap files pipes sockets time drm}

work=$(mktemp -d "${TMPDIR:-/tmp}/tunix-tests.XXXXXX")
trap 'rm -rf "$work"' EXIT INT TERM

base_memory=${MEMORY:-1G}
base_slack=${ROOT_SLACK_MIB:-32}
failed=
for name in $tests; do
	source=testsuites/kernel-tests/$name.c
	test -f "$source" || { echo "run.sh: no test called $name" >&2; exit 1; }
	dir=$work/$name
	tunix_root "$dir/root"
	$cc -std=gnu11 -Wall -Wextra -Werror -O2 -static -nostdlib -nostartfiles \
		-ffreestanding -fno-stack-protector -fno-pic -fno-pie -no-pie -fno-builtin \
		-fno-asynchronous-unwind-tables "$source" -o "$dir/root/sbin/init"
	memory=$base_memory
	slack=$base_slack
	wait=${TIMEOUT:-120}
	if [ "$name" = swap ]; then
		memory=320M
		slack=640
		wait=${TIMEOUT:-900}
	fi
	ROOT_SLACK_MIB=$slack
	export ROOT_SLACK_MIB
	tunix_image "$dir/tunix.img" "$dir/root"
	MEMORY=$memory
	export MEMORY
	tunix_boot "$dir/tunix.img" "$dir/serial.log" "$wait"
	if grep -a "^$name: " "$dir/serial.log" | tee "$dir/result" | grep -q "^$name: PASS"; then
		grep -a "^$name: PASS" "$dir/result"
	else
		grep -a "^$name: FAIL" "$dir/result" || { echo "$name: no result, last serial lines:"; tail -20 "$dir/serial.log"; }
		failed="$failed $name"
	fi
done

if [ -n "$failed" ]; then
	echo "failed:$failed"
	exit 1
fi
echo "all passed"
