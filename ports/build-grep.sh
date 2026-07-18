#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
source "$ROOT/ports/lib/gnu-port.sh"

PORT_NAME=grep
PORT_SRC="$ROOT/ports/src/grep"

# No libpcre2 in the static sysroot, so drop grep -P support.
PORT_CONFIGURE_ARGS=( --disable-perl-regexp )

grep_verify() {
    local bin="$1/usr/bin"
    [[ -x "$bin/grep" ]] || gnu_port_fail "grep binary missing"
    gnu_port_make_wrapper "$bin/egrep" grep -E
    gnu_port_make_wrapper "$bin/fgrep" grep -F
}
PORT_VERIFY=grep_verify

gnu_autotools_port
