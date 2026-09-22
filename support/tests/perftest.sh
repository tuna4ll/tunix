#!/bin/bash

CPUS=${1:-4}
KERNEL=${2:-build/kernel.elf}
ARCH=${ARCH:-x86_64}
BUILD=$(dirname "$KERNEL")
LIMINE_DIR=${LIMINE_DIR:-$BUILD/limine}
WORK=$BUILD/drmtest
IMAGE=$BUILD/perftest-$CPUS.img
LOG=$BUILD/perftest-$CPUS.log
WAIT=${WAIT:-240}
CC=cc
[ "$ARCH" = aarch64 ] && CC="aarch64-linux-gnu-gcc -mno-outline-atomics"

[ -f "$KERNEL" ] || { echo "perftest: $KERNEL is missing; run make kernel" >&2; exit 1; }
[ "$ARCH" != x86_64 ] || [ -x "$LIMINE_DIR/limine" ] || { echo "perftest: limine is missing; run make kernel" >&2; exit 1; }

rm -rf "$WORK"
mkdir -p "$WORK/root/sbin" "$WORK/root/dev" "$WORK/root/proc"
mkdir -p "$WORK/root/usr/share/weston/wallpapers"
cp base-files/overlay/usr/share/weston/wallpapers/tunix.png "$WORK/root/usr/share/weston/wallpapers/tunix.png"

mkdir -p "$WORK/root/files"
for i in $(seq 0 399); do
	printf %s "$(head -c 8192 /dev/zero | tr "\\0" "x")" > "$WORK/root/files/f$(printf %03d $i)"
done

$CC -std=gnu11 -Wall -Wextra -Werror -O2 -static -nostdlib -nostartfiles \
	-fno-stack-protector -fno-pic -fno-pie -fno-builtin -fno-asynchronous-unwind-tables \
	-DTUNIX_BENCH_CPUS="$CPUS" support/tests/perftest.c -o "$WORK/root/sbin/init" || exit 1

cat > "$WORK/limine.conf" <<CONF
timeout: 0
serial: yes

/Tunix
    protocol: limine
    path: boot():/boot/kernel.elf
    cmdline: root=LABEL=tunix-root ${EXTRA_CMDLINE:-}
CONF

ARCH=$ARCH TABLE=${IMAGE_TABLE:-gpt} ROOT_SLACK_MIB=16 \
	support/image.sh "$IMAGE" "$KERNEL" "$LIMINE_DIR" "$WORK/limine.conf" "$WORK/root" >/dev/null || exit 1

if [ "${BOOT:-1}" = 0 ]; then
	echo ":: $IMAGE ready -- write it with: sudo dd if=$IMAGE of=/dev/sdX bs=4M oflag=direct status=progress"
	exit 0
fi

rm -f "$LOG"
if [ "$ARCH" = aarch64 ]; then
	echo ":: booting $CPUS-processor aarch64 machine on tcg"
	timeout "$WAIT" qemu-system-aarch64 \
		-M virt,gic-version=3 -cpu cortex-a72 -smp "$CPUS" -m ${BENCH_MEMORY:-4G} \
		-kernel "$KERNEL" -append "root=LABEL=tunix-root ${EXTRA_CMDLINE:-}" \
		-device ramfb -drive "format=raw,file=$IMAGE,if=none,id=disk0" \
		-device nvme,drive=disk0,serial=tunix \
		-display none -no-reboot -serial "file:$LOG" >/dev/null 2>&1 &
else
	if [ -z "${ACCEL:-}" ]; then
		ACCEL=tcg
		[ -w /dev/kvm ] && ACCEL=kvm
	fi
	CPU=host
	[ "$ACCEL" = tcg ] && CPU=max
	echo ":: booting $CPUS-processor machine on $ACCEL"
	timeout "$WAIT" qemu-system-x86_64 \
		-machine "q35,accel=$ACCEL" -cpu "$CPU" -smp "$CPUS" -m ${BENCH_MEMORY:-4G} \
		${DRMTEST_GPU:-} -drive "format=raw,file=$IMAGE,if=none,id=disk0" \
		-device ide-hd,drive=disk0,bus=ide.0 \
		-display none -no-reboot -serial "file:$LOG" >/dev/null 2>&1 &
fi
QEMU=$!

for _ in $(seq "$WAIT"); do
	grep -aq "PERF DONE" "$LOG" 2>/dev/null && break
	kill -0 $QEMU 2>/dev/null || break
	sleep 1
done
kill $QEMU 2>/dev/null || true
wait $QEMU 2>/dev/null || true

grep -aE "^(PERF|SYSCALL|SMPCALL|PIPE|FAULT|FORK|FORKNOWAIT|THREAD|FILE|STARTUP|SHOOTDOWN|ONCE|ORPHAN|SYSLOG|MMAP|FUTEX|OOM|DF|KLOCK|KLOCKBOOT)" "$LOG" || {
	echo "perftest: the machine printed no results; $LOG has the boot" >&2
	exit 1
}

EXPECTED_WORKERS=$CPUS
[ "$EXPECTED_WORKERS" -le 4 ] || EXPECTED_WORKERS=4
SHARED_PEAK=$(sed -n "s/^SMPCALL workers=$EXPECTED_WORKERS .* shared_peak=\([0-9][0-9]*\)$/\1/p" "$LOG" | tail -1)
if [ "$EXPECTED_WORKERS" -gt 1 ] &&
   { [ -z "$SHARED_PEAK" ] || [ "$SHARED_PEAK" -lt "$EXPECTED_WORKERS" ]; }; then
	echo "perftest: only ${SHARED_PEAK:-0}/$EXPECTED_WORKERS processors overlapped in the kernel" >&2
	exit 1
fi

OFFSET=$(sfdisk -d "$IMAGE" 2>/dev/null |
	sed -n 's/^.*start= *\([0-9]*\).*type=\(83\|0FC63DAF\).*/\1/p' | head -1)
if [ -n "$OFFSET" ]; then
	for spec in mmapsync.bin:S mmapexit.bin:U; do
		file=${spec%%:*}
		want=${spec##*:}
		got=$(debugfs -R "cat /$file" "$IMAGE?offset=$(( OFFSET * 512 ))" 2>/dev/null |
			tr -d "\0" | head -c 8192)
		length=${#got}
		stray=$(printf %s "$got" | tr -d "$want" | wc -c)
		if [ "$length" = 8192 ] && [ "$stray" = 0 ]; then
			echo "ONDISK $file bytes=$length fill=$want PERSISTED"
		else
			echo "ONDISK $file bytes=$length unexpected=$stray fill=$want LOST"
		fi
	done
else
	echo "ONDISK could not find the root partition in $IMAGE" >&2
fi
