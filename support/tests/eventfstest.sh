#!/bin/sh
set -eu

binary="$(mktemp /tmp/tunix-eventfstest.XXXXXX)"
trap 'rm -f "$binary"' EXIT
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Ikernel/include \
    support/tests/eventfstest.c -o "$binary"
"$binary"
