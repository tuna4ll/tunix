#!/usr/bin/env python3
"""Split a concatenated PEM CA bundle into one file per certificate.

The image ships its trust as a single /etc/ssl/cert.pem, which is what GnuTLS
and mbedTLS were pointed at. OpenSSL's directory lookup wants the other layout:
a directory of individual certificates, found by a hash of the subject name
rather than by reading them all. Programs that ask OpenSSL for the default
*directory* -- xbps's bundled libfetch is one -- find nothing without it.

This only splits. The hash links are made afterwards by `openssl rehash`, which
knows how to compute them; there is no reason to reimplement that here.
"""
import pathlib
import sys

BEGIN = "-----BEGIN CERTIFICATE-----"
END = "-----END CERTIFICATE-----"


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: split-ca-bundle.py <bundle.pem> <output-directory>", file=sys.stderr)
        return 2

    bundle = pathlib.Path(sys.argv[1])
    destination = pathlib.Path(sys.argv[2])
    destination.mkdir(parents=True, exist_ok=True)

    written = 0
    current: list[str] = []
    for line in bundle.read_text().splitlines():
        if line.startswith(BEGIN):
            current = [line]
        elif current:
            current.append(line)
            if line.startswith(END):
                written += 1
                (destination / f"tunix-ca-{written:03d}.pem").write_text(
                    "\n".join(current) + "\n")
                current = []

    if not written:
        print(f"split-ca-bundle: no certificates found in {bundle}", file=sys.stderr)
        return 1
    print(f"CA bundle: {written} certificates written to {destination}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
