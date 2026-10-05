#!/bin/sh
exec env SHOW=UNIXDGRAM \
    support/tests/kerneltest.sh support/tests/unixdgram-kerneltest.c UNIXDGRAM \
    "${1:-build/kernel.elf}" "${2:-build/limine}"
