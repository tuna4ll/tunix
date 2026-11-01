#!/bin/sh
set -eu

. testsuites/boot.sh

tests=${*:-process memory swap zram files pipes sockets ipc time drm sound}

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
		slack=900
		wait=${TIMEOUT:-900}
	fi
	if [ "$name" = zram ]; then
		memory=320M
		wait=${TIMEOUT:-600}
	fi
	if [ "$name" = fuzz ]; then
		memory=512M
		wait=${TIMEOUT:-900}
	fi
	QEMU_EXTRA=
	if [ "$name" = sound ]; then
		if [ "$arch" = aarch64 ]; then
			module=${SOUND_MODULE:-kernel/build/aarch64-core/modules/snd_hda.ko}
		else
			module=${SOUND_MODULE:-kernel/build/modules/snd_hda.ko}
		fi
		cp "$module" "$dir/root/snd_hda.ko"
		QEMU_EXTRA="-audiodev wav,id=snd0,path=$dir/out.wav,out.frequency=48000,out.channels=2,out.format=s16 -audiodev none,id=snd1 -device intel-hda,id=hda0 -device hda-output,bus=hda0.0,audiodev=snd0 -device intel-hda,id=hda1 -device hda-duplex,bus=hda1.0,audiodev=snd1"
	fi
	export QEMU_EXTRA
	ROOT_SLACK_MIB=$slack
	export ROOT_SLACK_MIB
	tunix_image "$dir/tunix.img" "$dir/root"
	MEMORY=$memory
	export MEMORY
	tunix_boot "$dir/tunix.img" "$dir/serial.log" "$wait"
	if [ "$name" = sound ] && ! testsuites/kernel-tests/wavcheck.sh "$dir/out.wav" 90000 >>"$dir/serial.log"; then
		echo "sound: FAIL  the reference tone did not reach the audio device intact" >>"$dir/serial.log"
		sed -i "s/^sound: PASS/sound: FAIL/" "$dir/serial.log"
	fi
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
