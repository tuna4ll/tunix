#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
source "$ROOT/ports/lib/gnu-port.sh"

PORT_NAME=gawk
PORT_SRC="$ROOT/ports/src/gawk"

# Dynamic extensions need dlopen()/shared objects; MPFR/GMP are not in the
# static sysroot. Disable both so gawk links fully static.
PORT_CONFIGURE_ARGS=( --disable-extensions --disable-mpfr )

gawk_verify() {
    local bin="$1/usr/bin"
    [[ -x "$bin/gawk" ]] || gnu_port_fail "gawk binary missing"
    # gawk normally installs an `awk` link via an install hook; make sure the
    # generic name exists even though install-exec skips data-side links.
    [[ -e "$bin/awk" ]] || ln -s gawk "$bin/awk"
}
PORT_VERIFY=gawk_verify

gnu_autotools_port
