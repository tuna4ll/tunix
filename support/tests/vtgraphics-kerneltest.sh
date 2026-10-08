#!/bin/sh
exec env SHOW=VTGRAPHICS \
    support/tests/kerneltest.sh support/tests/vtgraphics-kerneltest.c VTGRAPHICS \
    "${1:-build/kernel.elf}" "${2:-build/limine}"
