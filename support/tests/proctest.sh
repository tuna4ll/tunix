#!/bin/sh
exec env CPUS="${CPUS:-2}" MEMORY=1G CFLAGS_EXTRA=-fno-tree-loop-distribute-patterns \
    support/tests/kerneltest.sh support/tests/proctest.c PROCTEST \
    "${1:-build/kernel.elf}" "${2:-build/limine}"
