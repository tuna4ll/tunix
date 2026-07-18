#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
source "$ROOT/ports/lib/gnu-port.sh"

PORT_NAME=diffutils
PORT_SRC="$ROOT/ports/src/diffutils"

diffutils_verify() {
    local bin="$1/usr/bin"
    [[ -x "$bin/diff" ]] || gnu_port_fail "diff binary missing"
    [[ -x "$bin/cmp" ]]  || gnu_port_fail "cmp binary missing"
}
PORT_VERIFY=diffutils_verify

gnu_autotools_port
