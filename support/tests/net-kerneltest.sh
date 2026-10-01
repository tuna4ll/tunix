#!/bin/sh
set -eu

kernel="${1:-build/kernel.elf}"
limine="${2:-build/limine}"
port=$((20000 + $$ % 20000))
work="$(mktemp -d /tmp/tunix-nettest.XXXXXX)"

cat > "$work/echo.py" <<'PY'
import socket, sys, threading
port = int(sys.argv[1])
udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
udp.bind(("127.0.0.1", port))
def datagrams():
    while True:
        data, peer = udp.recvfrom(65536)
        udp.sendto(data, peer)
threading.Thread(target=datagrams, daemon=True).start()
tcp = socket.socket()
tcp.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
tcp.bind(("127.0.0.1", port))
tcp.listen(4)
while True:
    connection, _ = tcp.accept()
    def serve(connection):
        while True:
            data = connection.recv(65536)
            if not data:
                break
            connection.sendall(data)
        connection.close()
    threading.Thread(target=serve, args=(connection,), daemon=True).start()
PY
python3 "$work/echo.py" "$port" &
server=$!
trap 'kill $server 2>/dev/null; rm -rf "$work"' EXIT

if [ "${ARCH:-x86_64}" = aarch64 ]; then
    device=virtio-net-pci
else
    device=virtio-net-pci,disable-legacy=on
fi
CFLAGS_EXTRA="-DHOST_PORT=$port" SHOW=NET WAIT="${WAIT:-300}" MEMORY="${MEMORY:-4G}" \
    QEMU_EXTRA="-netdev user,id=net0 -device $device,netdev=net0" \
    support/tests/kerneltest.sh support/tests/net-kerneltest.c NETTEST "$kernel" "$limine"
