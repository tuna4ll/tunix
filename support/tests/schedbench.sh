#!/bin/bash
# Build a machine whose whole userland is one static test, boot it, and print what it measured.
set -euo pipefail

CPUS=${1:-4}
KERNEL=${2:-build/kernel.elf}
BUILD=$(dirname "$KERNEL")
LIMINE_DIR=$BUILD/limine
WORK=$BUILD/schedbench
IMAGE=$BUILD/schedbench-$CPUS.img
LOG=$BUILD/schedbench-$CPUS.log

[ -f "$KERNEL" ] || { echo "schedbench: $KERNEL is missing; run make kernel" >&2; exit 1; }
[ -x "$LIMINE_DIR/limine" ] || { echo "schedbench: limine is missing; run make kernel" >&2; exit 1; }

rm -rf "$WORK"
mkdir -p "$WORK/root/sbin" "$WORK/root/dev" "$WORK/root/proc"

# -static and no libc at all, so that what the benchmark reports is the kernel
# with nothing in between.
cc -std=gnu11 -Wall -Wextra -Werror -O2 -static -nostdlib -nostartfiles \
	-fno-stack-protector -fno-pic -fno-pie -fno-builtin -fno-asynchronous-unwind-tables \
	-DTUNIX_BENCH_CPUS="$CPUS" \
	support/tests/schedbench.c -o "$WORK/root/sbin/init"

# A GPT disk with an ESP and a classic-ext2 root, with 16 MiB of slack rather
# than the 4 GiB an installable image wants.
cat > "$WORK/limine.conf" <<EOF
timeout: 0
serial: yes

/Tunix
    protocol: limine
    path: boot():/boot/kernel.elf
    cmdline: root=LABEL=tunix-root
EOF

TABLE=gpt ROOT_SLACK_MIB=16 \
	support/image.sh "$IMAGE" "$KERNEL" "$LIMINE_DIR" "$WORK/limine.conf" "$WORK/root" >/dev/null

# KVM where there is one, because a CR3 reload and a TLB refill are costs an emulator does not have.
ACCEL=tcg
[ -w /dev/kvm ] && ACCEL=kvm

echo ":: booting $CPUS-processor machine on $ACCEL"
rm -f "$LOG"
timeout 240 qemu-system-x86_64 \
	-machine "q35,accel=$ACCEL" -cpu host -smp "$CPUS" -m ${BENCH_MEMORY:-4G} \
	-drive "format=raw,file=$IMAGE,if=none,id=disk0" \
	-device ide-hd,drive=disk0,bus=ide.0 \
	-display none -no-reboot -serial "file:$LOG" >/dev/null 2>&1 &
QEMU=$!

# The benchmark says when it has finished, and waiting for that line rather than
# for the timeout is what keeps a run to the time it actually takes.
for _ in $(seq 240); do
	grep -q "BENCH DONE" "$LOG" 2>/dev/null && break
	kill -0 $QEMU 2>/dev/null || break
	sleep 1
done
kill $QEMU 2>/dev/null || true
wait $QEMU 2>/dev/null || true

grep -E "^(BENCH|NICE|QUANTUM|WAKE|SWITCH|PARALLEL|SLEEPER)" "$LOG" || {
	echo "schedbench: the machine printed no results; $LOG has the boot" >&2
	exit 1
}
