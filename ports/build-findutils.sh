#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
source "$ROOT/ports/lib/gnu-port.sh"

PORT_NAME=findutils
PORT_SRC="$ROOT/ports/src/findutils"

findutils_verify() {
    local bin="$1/usr/bin"
    [[ -x "$bin/find" ]]  || gnu_port_fail "find binary missing"
    [[ -x "$bin/xargs" ]] || gnu_port_fail "xargs binary missing"
}
PORT_VERIFY=findutils_verify

gnu_autotools_port
