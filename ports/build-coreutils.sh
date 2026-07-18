#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
source "$ROOT/ports/lib/gnu-port.sh"

PORT_NAME=coreutils
PORT_SRC="$ROOT/ports/src/coreutils/coreutils"

# FORCE_UNSAFE_CONFIGURE lets coreutils configure run as root (common in CI).
PORT_CONFIGURE_ENV=( FORCE_UNSAFE_CONFIGURE=1 )
# stdbuf needs an LD_PRELOAD shared library, useless in a static image; uptime
# is already provided by the Tunix-native procutil tool in /bin.
PORT_CONFIGURE_ARGS=( --enable-no-install-program=stdbuf,uptime )

coreutils_verify() {
    local bin="$1/usr/bin"
    for b in ls cat cp mv rm mkdir chmod echo test; do
        [[ -x "$bin/$b" ]] || gnu_port_fail "coreutils did not install $b"
    done
    [[ -e "$bin/[" ]] || gnu_port_fail "coreutils did not install the [ test binary"
}
PORT_VERIFY=coreutils_verify

gnu_autotools_port
