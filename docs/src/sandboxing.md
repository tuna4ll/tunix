# Sandboxing

What a browser's content process needs to lock itself down: a syscall filter and
a user namespace. Both are real here — a filtered syscall is really refused, and
a user namespace really remaps the uid — so Firefox stops warning that its
sandbox is disabled and starts its content processes with one.

## seccomp-bpf

`seccomp(2)` and `prctl(PR_SET_SECCOMP)` install a classic-BPF program that runs
before every syscall. The interpreter and its verifier are in
`kernel/core/seccomp.c`; the verifier rejects a program with a bad opcode, an
out-of-range jump or a stray memory slot before it ever runs, and the
interpreter is bounded so a filter cannot read past the `seccomp_data` it is
handed (`nr`, `arch`, the instruction pointer and six arguments).

A filter returns an action, and the most restrictive one across all installed
filters wins:

| Action | Effect |
| --- | --- |
| `SECCOMP_RET_ALLOW` | the syscall runs |
| `SECCOMP_RET_ERRNO` | the syscall is skipped and returns `-errno` |
| `SECCOMP_RET_TRAP` | a `SIGSYS` is delivered to the process's handler |
| `SECCOMP_RET_KILL_THREAD` / `KILL_PROCESS` | the process dies on `SIGSYS` |
| `SECCOMP_RET_TRACE` / `USER_NOTIF` | no tracer or listener, so `-ENOSYS` |
| `SECCOMP_RET_LOG` | the syscall runs |

`SECCOMP_MODE_STRICT` is the short form: only `read`, `write`, `exit`,
`exit_group` and `rt_sigreturn`, everything else kills the process. A filter may
only be installed by root or by a task that has set `PR_SET_NO_NEW_PRIVS`, and
`fork`/`clone` inherit the filter chain (reference-counted, freed on exit). The
enforcement point is the top of `syscall_run`, keyed on the native syscall
number so the arch the filter was built for matches the arch field it reads.

The interpreter takes untrusted input, so it is fuzzed on the host under
ASan/UBSan — about 14 million random programs, including valid-opcode-weighted
ones that pass the verifier, with no out-of-bounds access. `make test
TESTS=seccomp` is the in-kernel check.

## User namespaces

`unshare(CLONE_NEWUSER)` or `clone(CLONE_NEWUSER)` makes a new user namespace
whose creator may then write `/proc/self/uid_map` and `/proc/self/gid_map` (once
each, after `/proc/self/setgroups` is set to `deny` for the gid map). The maps
are real: a process that ran as uid 1000 and maps `0 1000 1` reads back uid 0
from `getuid`/`geteuid` inside the namespace, which is exactly how an
unprivileged browser becomes "root" in its own sandbox without any real
privilege on the host. `kernel/core/namespace.c` keeps the extents (up to five),
inverts them for the read-back, and refuses a second write or a map before the
namespace exists.

Creating a user namespace needs no privilege — that is the point — while any
other namespace flag requires a capability in the current one. Mapping covers
`getuid`, `geteuid`, `getgid` and `getegid`; file ownership stays global.

## UTS namespaces

`CLONE_NEWUTS` gives a process its own `nodename` and `domainname`, copied from
the parent namespace, so `sethostname` inside it does not change what the rest of
the system sees through `uname`. The root UTS namespace is the global hostname,
so nothing outside a sandbox changes.

## What is not here yet

PID, network, mount, IPC and cgroup namespaces are **refused, not faked**:
`clone`/`unshare` returns `-EINVAL` for them rather than pretend to isolate.
Firefox falls back to fewer namespaces when they are missing and still sandboxes
its content processes with seccomp and the user namespace; Chromium's zygote
wants the others, so it needs `--no-sandbox` until they land.

`make test TESTS=namespace` is the in-kernel check.
