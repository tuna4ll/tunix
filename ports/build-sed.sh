#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
source "$ROOT/ports/lib/gnu-port.sh"

PORT_NAME=sed
PORT_SRC="$ROOT/ports/src/sed"

sed_verify() {
    [[ -x "$1/usr/bin/sed" ]] || gnu_port_fail "sed binary missing"
}
PORT_VERIFY=sed_verify

gnu_autotools_port
