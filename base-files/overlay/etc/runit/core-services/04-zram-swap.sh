zram_swap() {
	grep -qw nozram /proc/cmdline && return 0
	[ -w /sys/block/zram0/disksize ] || return 0
	[ "$(cat /sys/block/zram0/disksize)" = 0 ] || return 0
	memory=$(awk '/^MemTotal:/ { print $2 }' /proc/meminfo)
	[ -n "$memory" ] || return 0
	size=$((memory / 2))
	[ "$size" -gt 4194304 ] && size=4194304
	echo "${size}K" > /sys/block/zram0/disksize || return 0
	mkswap /dev/zram0 >/dev/null 2>&1 || return 0
	swapon -p 100 /dev/zram0 && msg "Enabled ${size} KiB of compressed swap on zram0."
}

zram_swap
unset -f zram_swap
unset memory size
