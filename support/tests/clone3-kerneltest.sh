#!/bin/sh
exec env X86_CFLAGS=-mstackrealign \
    support/tests/kerneltest.sh support/tests/clone3-kerneltest.c CLONE3TEST \
    "${1:-build/kernel.elf}" "${2:-build/limine}"
