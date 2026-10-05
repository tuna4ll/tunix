#!/bin/sh
exec env CPUS="${CPUS:-2}" SHOW=CGROUPTEST \
    support/tests/kerneltest.sh support/tests/cgroup-kerneltest.c CGROUPTEST \
    "${1:-build/kernel.elf}" "${2:-build/limine}"
