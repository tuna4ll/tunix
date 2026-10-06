#!/bin/sh
exec env CPUS="${CPUS:-2}" SHOW=ACLTEST \
    support/tests/kerneltest.sh support/tests/acl-kerneltest.c ACLTEST \
    "${1:-build/kernel.elf}" "${2:-build/limine}"
