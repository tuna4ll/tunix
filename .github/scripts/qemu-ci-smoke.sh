#!/usr/bin/env bash
set -euo pipefail

image=${1:-build/tunix.img}
log=${2:-build/qemu-ci.log}
timeout_seconds=${QEMU_CI_TIMEOUT:-45}
qemu=${QEMU:-qemu-system-x86_64}
success_marker=${TUNIX_CI_SUCCESS_MARKER:-TUNIX_CI_BOOT_OK}

mkdir -p "$(dirname "$log")"
rm -f "$log"

"$qemu" -machine pc -m 128M -drive "format=raw,file=$image" \
    -nographic -monitor none -serial stdio -no-reboot -no-shutdown \
    -netdev user,id=net0 -device rtl8139,netdev=net0 \
    >"$log" 2>&1 &
qemu_pid=$!

cleanup() {
    if kill -0 "$qemu_pid" >/dev/null 2>&1; then
        kill "$qemu_pid" >/dev/null 2>&1 || true
        wait "$qemu_pid" >/dev/null 2>&1 || true
    fi
}
trap cleanup EXIT

for ((elapsed = 0; elapsed < timeout_seconds; elapsed++)); do
    if grep -Fq "$success_marker" "$log" 2>/dev/null; then
        echo "qemu-ci: boot marker found: $success_marker"
        exit 0
    fi
    if ! kill -0 "$qemu_pid" >/dev/null 2>&1; then
        wait "$qemu_pid" || true
        break
    fi
    sleep 1
done

echo "qemu-ci: boot marker not found within ${timeout_seconds}s: $success_marker" >&2
echo "qemu-ci: last serial output:" >&2
tail -n 80 "$log" >&2 || true
exit 1
