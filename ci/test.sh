#!/bin/sh
set -eu

arch=${1:-x86_64}
case $arch in
x86_64) make -j"$(nproc)" test && make ci-boot ;;
aarch64) make -j"$(nproc)" test-aarch64 && make ci-boot-aarch64 ;;
*) echo "test.sh: arch must be x86_64 or aarch64, not $arch" >&2; exit 1 ;;
esac
