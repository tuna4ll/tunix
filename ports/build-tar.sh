#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
source "$ROOT/ports/lib/gnu-port.sh"

PORT_NAME=tar
PORT_SRC="$ROOT/ports/src/tar"

# Allow tar's configure to run as root in CI.
PORT_CONFIGURE_ENV=( FORCE_UNSAFE_CONFIGURE=1 )

tar_verify() {
    [[ -x "$1/usr/bin/tar" ]] || gnu_port_fail "tar binary missing"
}
PORT_VERIFY=tar_verify

gnu_autotools_port
