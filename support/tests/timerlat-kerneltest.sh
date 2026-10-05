#!/bin/sh
exec env CPUS="${CPUS:-4}" SHOW=TIMERLAT \
    support/tests/kerneltest.sh support/tests/timerlat-kerneltest.c TIMERLAT \
    "${1:-build/kernel.elf}" "${2:-build/limine}"
