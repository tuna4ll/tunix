# EventFS

EventFS exposes high-level kernel changes as live Unix text streams under
`/events`. It does not retain history and it is not a tracing facility.

The fixed entries are `process`, `files`, `devices`, and `network`. Opening an
entry creates one subscriber, and descriptors duplicated from that open file
description share its subscriber. A separate `open()` gets a separate queue.

Each subscriber owns an 8192-byte bounded ring of length-prefixed records. A
producer copies a complete formatted record into every matching subscriber and
never stores subsystem pointers. Producers allocate no memory and never wait
for slow readers. Once a queue cannot accept a record, later records are counted
until the reader drains the older records and receives `lost N`.

The maximum record size is 4096 bytes. Every record ends in a newline. Reads can
return several whole records, but a buffer smaller than the first record gets
`EMSGSIZE` without consuming it. An empty queue gets `EAGAIN`; the syscall layer
turns that into scheduler sleep unless the descriptor has `O_NONBLOCK`.

Read readiness includes queued records and pending loss reports. EventFS uses a
per-subscriber wait channel, while Tunix poll, select, and epoll also wake from
the kernel's shared I/O wait channel.

The kernel lock serializes subscription, publication, reading, and close. Its
lock order is kernel lock followed by the process wake oplock. Formatting,
allocation, user copies, and scheduler sleep never happen while an EventFS
queue lock is held because EventFS has no second queue lock. A close unlinks and
wakes a subscriber before freeing it, clearing any sleeping process's channel.

Root subscribers receive all records. Other subscribers only receive process,
file, and network records whose captured effective UID equals the opener's
effective UID. Device records are root-only. This conservative policy avoids
exposing system-wide activity until Tunix has a richer authorization model.

Fields use backslash escaping. Space, tab, newline, carriage return, and
backslash become `\ `, `\t`, `\n`, `\r`, and `\\`. Other ASCII control bytes
become `\xhh`, and an empty field becomes `\0`. A source newline therefore
cannot inject another event record.

## Current scope

Process hooks cover exec, fork, exit, signal queueing, and user-mode faults.
File hooks cover successful regular-file create, write, truncate-on-open,
rename, and remove syscalls; page-cache and block writes do not create events.
IPv4 TCP connect, accept, and close are covered, as are UDP connect and close.

Block-device and partition registration emits `attach`. Tunix currently has no
block or USB unregister/hot-remove path, so the `remove` producer API exists but
has no fabricated call site. A future unregister path must emit before freeing
its device metadata.

All current producer paths are serialized by the exclusive kernel lock. A
future subsystem allowed to publish from a shared-lock or lockless context must
first add a per-channel lock or deferred publication rather than calling the
current API directly.

Run the queue and protocol unit tests from the repository root:

```sh
support/tests/eventfstest.sh
```

Run the in-kernel blocking, poll, overflow, and close-race test against a built
kernel with:

```sh
support/tests/eventfs-kerneltest.sh build/kernel.elf build/limine
```
