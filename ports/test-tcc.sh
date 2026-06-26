#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
TCC_ROOT=${TCC_ROOT:-$ROOT/ports/out/tcc-root}
TCC_BIN="$TCC_ROOT/usr/bin/tcc"

fail() {
    echo "test-tcc: $*" >&2
    exit 1
}

[[ -x "$TCC_BIN" ]] || fail "compiler not found at $TCC_BIN"
command -v python3 >/dev/null 2>&1 || fail "python3 was not found"

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

"$TCC_BIN" -nostdinc \
    -I"$TCC_ROOT/usr/lib/tcc/include" \
    -I"$TCC_ROOT/usr/include" \
    -c "$ROOT/tests/tcc/headers.c" \
    -o "$work/headers.o"

"$TCC_BIN" -nostdlib -static -Wl,-e,_start \
    "$ROOT/tests/tcc/freestanding.c" \
    -o "$work/freestanding"

python3 - "$work/headers.o" "$work/freestanding" <<'PY'
import struct
import sys

for path in sys.argv[1:]:
    with open(path, 'rb') as handle:
        header = handle.read(64)
    if len(header) < 64 or header[:4] != b'\x7fELF':
        raise SystemExit(f'{path}: not an ELF file')
    if header[4] != 2 or header[5] != 1:
        raise SystemExit(f'{path}: expected ELF64 little-endian output')
    e_type, e_machine = struct.unpack_from('<HH', header, 16)
    if e_machine != 62:
        raise SystemExit(f'{path}: expected x86_64 machine, got {e_machine}')
    if path.endswith('freestanding') and e_type != 2:
        raise SystemExit(f'{path}: expected ET_EXEC, got {e_type}')
PY

output=$($work/freestanding)
[[ "$output" == "tcc smoke ok" ]] || fail "unexpected executable output: $output"

echo "TinyCC compile, link and execute smoke tests passed"
