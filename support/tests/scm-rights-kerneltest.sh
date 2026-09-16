#!/bin/sh
exec support/tests/kerneltest.sh support/tests/scm-rights-kerneltest.c SCMRIGHTSTEST \
    "${1:-build/kernel.elf}" "${2:-build/limine}"
