#!/bin/sh
set -eu

jobs=$(nproc)
make -j"$jobs" check
make -j"$jobs" aarch64-core modules-aarch64
make acpi-test
