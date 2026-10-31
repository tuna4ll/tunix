arch=${ARCH:-x86_64}
limine=${LIMINE:-kernel/build/limine}

if [ "$arch" = aarch64 ]; then
	kernel=${KERNEL:-kernel/build/kernel-aarch64-core.img}
	cc=${AARCH64_CC:-aarch64-linux-gnu-gcc}
	cxx=${AARCH64_CXX:-aarch64-linux-gnu-g++}
else
	kernel=${KERNEL:-kernel/build/kernel.elf}
	cc=${CC:-cc}
	cxx=${CXX:-c++}
fi

test -f "$kernel" || { echo "no kernel at $kernel" >&2; exit 1; }

tunix_root() {
	mkdir -p "$1/sbin" "$1/dev" "$1/proc" "$1/tmp"
}

tunix_image() {
	_dir=$(dirname "$1")
	printf '%s\n' 'timeout: 0' 'serial: yes' '/Tunix' '    protocol: limine' \
		'    path: boot():/boot/kernel.elf' '    cmdline: root=LABEL=tunix-root' >"$_dir/limine.conf"
	ARCH=$arch TABLE=gpt ROOT_SLACK_MIB=${ROOT_SLACK_MIB:-32} tools/image.sh "$1" "$kernel" \
		"$limine" "$_dir/limine.conf" "$2" >"$_dir/image.log" 2>&1 ||
		{ cat "$_dir/image.log" >&2; return 1; }
}

tunix_boot() {
	_wait=${3:-120}
	_cpus=${CPUS:-2}
	if [ "$arch" = aarch64 ]; then
		timeout "$_wait" qemu-system-aarch64 -M virt,gic-version=3 -cpu "${AARCH64_CPU:-cortex-a72}" \
			-smp "$_cpus" -m "${MEMORY:-1G}" -kernel "$kernel" -append root=LABEL=tunix-root \
			-drive "format=raw,file=$1,if=none,id=disk0" -device nvme,drive=disk0,serial=tunix \
			-display none -no-reboot -serial "file:$2" ${QEMU_EXTRA:-} >/dev/null 2>&1 || true
	else
		timeout "$_wait" qemu-system-x86_64 -machine q35,accel=kvm:tcg -cpu max \
			-smp "$_cpus" -m "${MEMORY:-1G}" -drive "format=raw,file=$1,if=none,id=disk0" \
			-device ide-hd,drive=disk0,bus=ide.0 -display none -no-reboot \
			-serial "file:$2" ${QEMU_EXTRA:-} >/dev/null 2>&1 || true
	fi
}
