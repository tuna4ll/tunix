#!/bin/sh
exec env CPUS="${CPUS:-4}" SHOW=WAKELAT ROOT_SLACK_MIB="${ROOT_SLACK_MIB:-96}" WAIT="${WAIT:-300}" \
    support/tests/kerneltest.sh support/tests/wakelat-kerneltest.c WAKELAT \
    "${1:-build/kernel.elf}" "${2:-build/limine}"
