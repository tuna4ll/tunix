#!/bin/sh
extra=
[ "${ARCH:-x86_64}" = aarch64 ] && extra=-mno-outline-atomics
exec env CPUS="${CPUS:-4}" WAIT="${WAIT:-600}" CFLAGS_EXTRA="$extra" \
    support/tests/kerneltest.sh support/tests/smp-stress-kerneltest.c SMPSTRESS \
    "${1:-build/kernel.elf}" "${2:-build/limine}"
