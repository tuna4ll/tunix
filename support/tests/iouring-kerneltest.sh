#!/bin/sh
exec env SHOW=IOURING \
    support/tests/kerneltest.sh support/tests/iouring-kerneltest.c IOURING \
    "${1:-build/kernel.elf}" "${2:-build/limine}"
