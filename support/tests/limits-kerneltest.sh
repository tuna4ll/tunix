#!/bin/sh
exec env CPUS="${CPUS:-2}" SHOW=LIMITS WAIT="${WAIT:-300}" \
    support/tests/kerneltest.sh support/tests/limits-kerneltest.c LIMITSTEST \
    "${1:-build/kernel.elf}" "${2:-build/limine}"
