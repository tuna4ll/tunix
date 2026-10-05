#!/bin/sh
set -eu
cold="$(mktemp -d /tmp/tunix-vfslat.XXXXXX)"
trap 'rm -rf "$cold"' EXIT
head -c "$(( ${COLD_MIB:-64} << 20 ))" /dev/urandom > "$cold/cold.bin"
env CPUS="${CPUS:-4}" SHOW=VFSLAT ROOT_FILES="$cold/cold.bin" \
    ROOT_SLACK_MIB="$(( ${COLD_MIB:-64} + 32 ))" WAIT="${WAIT:-300}" \
    support/tests/kerneltest.sh support/tests/vfslat-kerneltest.c VFSLAT \
    "${1:-build/kernel.elf}" "${2:-build/limine}"
