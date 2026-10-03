#!/bin/sh
exec env CPUS=4 SHOW=PTYPOLL \
    support/tests/kerneltest.sh support/tests/ptypoll-kerneltest.c PTYPOLL \
    "${1:-build/kernel.elf}" "${2:-build/limine}"
