#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
source "$ROOT/ports/lib/gnu-port.sh"

PORT_NAME=gzip
PORT_SRC="$ROOT/ports/src/gzip"

gzip_verify() {
    local bin="$1/usr/bin"
    [[ -x "$bin/gzip" ]] || gnu_port_fail "gzip binary missing"
    # gunzip/zcat ship as shell scripts (data), skipped by install-exec.
    gnu_port_make_wrapper "$bin/gunzip" gzip -d
    gnu_port_make_wrapper "$bin/zcat" gzip -cd
}
PORT_VERIFY=gzip_verify

gnu_autotools_port
