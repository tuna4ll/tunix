# Fuzzing

What the kernel exposes so a fuzzer can drive it and make sense of a crash:
coverage feedback, a heap sanitiser, symbolised oops reports, and a syscall
fuzzer in the test suite. None of it is in the default build; each is a compile
flag, so a shipped kernel carries none of the cost.

## Coverage: KCOV

`KCOV=1` compiles the portable kernel with
`-fsanitize-coverage=trace-pc,trace-cmp`. Every basic block then calls
`__sanitizer_cov_trace_pc`, and every comparison calls a `trace_cmp` hook; both
live in `kernel/core/kcov.c`. The low-level code that a trace would recurse
through — the allocators, the page tables, the console, the exception path — is
excluded in `SANITIZE_EXCLUDE`, so a hook never runs inside the code that serves
it.

The interface is Linux's. A task opens `/sys/kernel/debug/kcov`, sizes a buffer
with `KCOV_INIT_TRACE`, maps it, and switches collection on with `KCOV_ENABLE`
(`KCOV_TRACE_PC` for block addresses, `KCOV_TRACE_CMP` for comparison operands).
While it is on, only that task's own kernel paths are recorded, into its own
buffer; word zero is the count and the rest is the trace. `KCOV_DISABLE` stops
it, closing the descriptor keeps an enabled trace alive until the task exits,
and the buffer stops growing at its last word rather than overrunning. This is
exactly the shape `syz-executor` drives, so an external syzkaller run gets
coverage with no further kernel work.

```
make test TESTS=kcov KCOV=1       # the KCOV self-tests, both arches
```

## The heap sanitiser: KASAN

`KASAN=1` compiles with `-fsanitize=kernel-address` in outline mode: each memory
access calls `__asan_load*`/`__asan_store*` in `kernel/mm/kasan.c`, which checks
a shadow byte for every eight bytes of the heap. The shadow lives in its own
window (`KASAN_SHADOW_BASE`), is mapped a page at a time as the heap grows, and
is shared by every address space because it sits under the same top-level entry
as the kernel image.

`kmalloc` leaves a poisoned redzone after each object and `kfree` poisons the
whole object and parks it in a quarantine, so a read or write past an object, a
use after free, and a double free each land on a poisoned byte and produce a
`BUG: KASAN:` report naming the function. The allocator, the page tables and the
string routines are excluded, as with KCOV.

```
make test TESTS=kcov KASAN=1               # build with the sanitiser on
# boot any KASAN kernel with `kasantest=1` on the command line to run
# kernel/mm/kasan_test.c, which trips all three bugs on purpose and checks
# each one is reported: "KASAN-SELFTEST: 3/3 reported".
```

## Readable crashes

A fuzzer reads the console to tell a crash from noise, so the kernel prints the
way Linux does. `kernel/core/kallsyms.c` carries a symbol table built at link
time by `kernel/scripts/kallsyms.sh` (the link runs twice so the table cannot
move the code it names), and `/proc/kallsyms` serves it — zeroed for an
unprivileged reader, as on Linux. On a fault the kernel prints
`BUG: unable to handle...` or `general protection fault`, the registers, and a
`Call Trace:` whose frames are symbolised (`function+0xoffset/0xsize`, with
`[module]` for a loadable driver); a panic ends `Kernel panic - not syncing:`.
Build with `FRAME_POINTERS=1` and the trace walks the saved frames exactly
instead of scanning the stack. The lock checker reports contention and bad
unlocks as `WARNING:`/`BUG:` with a trace too. syzkaller's Linux report parser
recognises all of these unchanged.

## The built-in syscall fuzzer

`testsuites/kernel-tests/fuzz.c` is a small fuzzer that runs as init: it forks
workers that each seed a handful of descriptors (a pipe, a file, a socket, an
eventfd, an epoll, a memfd) and then fire thousands of random syscalls with a
mix of valid descriptors, buffer pointers, paths and junk. The supervisor
ignores the catchable signals so a stray `kill` cannot take it down, each worker
arms a three-second timer so a blocking call cannot wedge it, and the
self-destructive calls (`munmap`, `clone`, the timer setters) are held back so a
worker keeps running. A worker that faults is reaped; the run fails only if the
kernel itself does — a panic, a KASAN report, a lock warning.

```
make test TESTS=fuzz            # default build
make test TESTS=fuzz KASAN=1    # with the sanitiser watching the heap
```

It is deliberately not in the default `make test` set: it is slow, and under
TCG on aarch64 slower still. Run against a `KASAN=1` kernel it is the sharpest
check in the tree — a few hundred thousand random calls across every subsystem
with the heap watched the whole time.

## Driving syzkaller

syzkaller's own `linux/amd64` target drives Tunix unchanged, because the syscall
ABI and all four of the pieces above are the ones it expects: it finds
`/sys/kernel/debug/kcov`, parses `/proc/kallsyms` and the oops format, and sees
the KASAN reports. Point a `syz-manager` at a Tunix image with an ssh server and
a statically linked `syz-executor`, built for `linux/amd64`; syscalls Tunix does
not implement return `ENOSYS` and syzkaller disables them on its own. The kernel
side needs nothing beyond `KCOV=1` (and `KASAN=1` to catch silent corruption).
