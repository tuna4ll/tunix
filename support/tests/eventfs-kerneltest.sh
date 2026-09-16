#!/bin/sh
exec env CPUS=4 SHOW=EVENTFS \
    support/tests/kerneltest.sh support/tests/eventfs-kerneltest.c EVENTFSTEST \
    "${1:-build/kernel.elf}" "${2:-build/limine}"
