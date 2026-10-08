#!/bin/sh
exec env SHOW=UNIXLISTEN \
    support/tests/kerneltest.sh support/tests/unixlisten-kerneltest.c UNIXLISTEN \
    "${1:-build/kernel.elf}" "${2:-build/limine}"
