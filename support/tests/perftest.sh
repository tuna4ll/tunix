#!/bin/bash
# Build a machine whose whole userland is one static test, boot it, and print what it measured.
# Build a machine whose whole userland is the performance test, boot it, and print what it found.

CPUS=${1:-4}
KERNEL=${2:-build/kernel.elf}
BUILD=$(dirname "$KERNEL")
LIMINE_DIR=$BUILD/limine
WORK=$BUILD/drmtest
IMAGE=$BUILD/perftest-$CPUS.img
LOG=$BUILD/perftest-$CPUS.log

[ -f "$KERNEL" ] || { echo "drmtest: $KERNEL is missing; run make kernel" >&2; exit 1; }
[ -x "$LIMINE_DIR/limine" ] || { echo "drmtest: limine is missing; run make kernel" >&2; exit 1; }

rm -rf "$WORK"
mkdir -p "$WORK/root/sbin" "$WORK/root/dev" "$WORK/root/proc"

# Several hundred small files, because opening that many is what a startup
# does and every one of them is inodes and directory blocks off the medium.
mkdir -p "$WORK/root/files"
for i in $(seq 0 399); do
	printf %s "$(head -c 8192 /dev/zero | tr "\\0" "x")" > "$WORK/root/files/f$(printf %03d $i)"
done

# -static and no libc at all, so that what the benchmark reports is the kernel
# with nothing in between.
cc -std=gnu11 -Wall -Wextra -Werror -O2 -static -nostdlib -nostartfiles \
	-fno-stack-protector -fno-pic -fno-pie -fno-builtin -fno-asynchronous-unwind-tables \
	\
	support/tests/perftest.c -o "$WORK/root/sbin/init"

# A GPT disk with an ESP and a classic-ext2 root, with 16 MiB of slack rather
# than the 4 GiB an installable image wants.
cat > "$WORK/limine.conf" <<EOF
timeout: 0
serial: yes

/Tunix
    protocol: limine
    path: boot():/boot/kernel.elf
    cmdline: root=LABEL=tunix-root ${EXTRA_CMDLINE:-}
EOF

TABLE=${IMAGE_TABLE:-gpt} ROOT_SLACK_MIB=16 \
	support/image.sh "$IMAGE" "$KERNEL" "$LIMINE_DIR" "$WORK/limine.conf" "$WORK/root" >/dev/null

# BOOT=0 stops with the image built, for writing to a stick and booting a real
# machine; the results land in a file on its root filesystem.
if [ "${BOOT:-1}" = 0 ]; then
	echo ":: $IMAGE ready -- write it with: sudo dd if=$IMAGE of=/dev/sdX bs=4M oflag=direct status=progress"
	exit 0
fi

# KVM where there is one, because a CR3 reload and a TLB refill are costs an emulator does not have.
ACCEL=tcg
[ -w /dev/kvm ] && ACCEL=kvm

echo ":: booting $CPUS-processor machine on $ACCEL"
rm -f "$LOG"
timeout 240 qemu-system-x86_64 \
	-machine "q35,accel=$ACCEL" -cpu host -smp "$CPUS" -m ${BENCH_MEMORY:-4G} \
	${DRMTEST_GPU:-} -drive "format=raw,file=$IMAGE,if=none,id=disk0" \
	-device ide-hd,drive=disk0,bus=ide.0 \
	-display none -no-reboot -serial "file:$LOG" >/dev/null 2>&1 &
QEMU=$!

# The benchmark says when it has finished, and waiting for that line rather than
# for the timeout is what keeps a run to the time it actually takes.
for _ in $(seq 240); do
	grep -q "PERF DONE" "$LOG" 2>/dev/null && break
	kill -0 $QEMU 2>/dev/null || break
	sleep 1
done
kill $QEMU 2>/dev/null || true
wait $QEMU 2>/dev/null || true

grep -E "^(PERF|SYSCALL|PIPE|FAULT|FORK|FORKNOWAIT|THREAD|FILE|STARTUP|SHOOTDOWN|ONCE|ORPHAN|SYSLOG|DF|KLOCK|KLOCKBOOT)" "$LOG" || {
	echo "drmtest: the machine printed no results; $LOG has the boot" >&2
	exit 1
}
