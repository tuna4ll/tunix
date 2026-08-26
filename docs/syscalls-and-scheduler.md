# Syscalls and Scheduler

This document covers how a syscall gets from user space into the kernel, how
the dispatch table is organized, and how the scheduler picks and switches
between processes. It reflects the code as it exists today, not a target
design.

## Syscall ABI

Tunix reuses the Linux x86_64 syscall numbers and calling convention:

- Syscall number in `rax`, return value in `rax`.
- Arguments in `rdi`, `rsi`, `rdx`, `r10`, `r8`, `r9` (in that order). `r10` is
  used instead of `rcx` for the fourth argument because the `SYSCALL`
  instruction itself clobbers `rcx` (return `rip`) and `r11` (saved
  `rflags`).
- Invoked with the `syscall` instruction from user space.

Reusing the Linux numbering means unmodified x86_64 binaries can issue
syscalls Tunix already understands without a translation layer. Numbers and
handlers are defined in `kernel/syscall.c:46` (the `SYS_*` `#define`
block).

## Entry path

`syscall_init` (`kernel/syscall.c:527`) programs the syscall MSRs once at
boot:

- `EFER.SCE` (MSR `0xC0000080`) is set to enable the `SYSCALL`/`SYSRET`
  instructions, plus the NX bit if the CPU supports it.
- `STAR` (MSR `0xC0000081`) sets the kernel/user code segment selectors used
  on entry/exit.
- `LSTAR` (MSR `0xC0000082`) points at `syscall_entry`, the raw entry point.
- `FMASK` (MSR `0xC0000084`) clears `IF` and `DF` on entry.

`syscall_entry` (`kernel/arch/x86_64/syscall_entry.S:20`) is hand-written
assembly, not a C function, because it runs before there is a valid kernel
stack:

1. It `swapgs`es to bring this processor's own block into reach, stashes the
   incoming user `rsp` there, and switches to the kernel stack the block
   names (set by `syscall_set_kernel_stack` — see below). A global would not
   do: which stack to switch to is a different answer on every processor, and
   the question has to be answered before there is a stack to answer it with.
2. It spills all argument/callee-relevant registers plus `rcx`/`r11`
   (the `SYSCALL`-clobbered return `rip`/`rflags`) and the saved user `rsp`
   into a `struct syscall_frame` (`kernel/include/syscall.h:6`) on that
   stack.
3. It calls `syscall_dispatch(frame)`.
4. On return, it rebuilds an `iretq` frame from the (possibly modified)
   `syscall_frame`, `swapgs`es back, and returns to user space via `iretq`
   rather than `sysretq`.

Using `iretq` for every return — even the ordinary `SYSCALL` fast path — is
deliberate: it lets the scheduler resume a process with one common frame
format regardless of whether it was last suspended by a syscall or by the
timer interrupt (see below). `process_enter_user`
(`kernel/arch/x86_64/syscall_entry.S:87`) uses the same `iretq` technique
for the very first entry into user space.

Two separate "kernel stack" pointers exist and both are updated together
whenever the scheduler switches processes (`activate_process`,
`kernel/process.c:397`):

- `tss.rsp0`, set via `set_kernel_stack` (`kernel/arch/x86_64/gdt.c`) —
  used by the CPU for privilege-level changes on interrupts/exceptions
  (e.g. the timer IRQ).
- `kernel_rsp` in the per-CPU block, set via `syscall_set_kernel_stack` —
  used explicitly by `syscall_entry` because `SYSCALL` does not consult the
  TSS.

Both are per-processor, and for the same reason: they name the kernel stack of
whichever process *this* processor is running.

## Dispatch table

`syscall_dispatch` takes the kernel lock, and `syscall_dispatch_locked`
(`kernel/syscall.c`) is a single `switch` on `frame->rax`. On every call it
first accounts CPU time for the caller (`process_account_runtime`) and frees
any processes left in `PROCESS_DEAD` state by a previous switch
(`process_reap_deferred`), then dispatches. Implemented syscalls fall into
rough groups:

- **File I/O**: `read`, `write`, `open(at)`, `close`, `lseek`, `pread64`/
  `pwrite64`, `readv`/`writev`, `stat`/`fstat`/`lstat`/`newfstatat`,
  `getdents64`, `ioctl`, `fcntl`, `dup`/`dup2`/`dup3`.
- **Filesystem namespace**: `mkdir(at)`, `rmdir`, `unlink(at)`, `rename(at)`,
  `chdir`/`fchdir`/`getcwd`, `chmod(at)`/`fchmod`, `readlink(at)`,
  `symlinkat`, `access(at)`, `umask`.
- **Memory**: `brk`, `mmap`, `mprotect`, `munmap`.
- **Process/thread lifecycle**: `fork`, `vfork`, `clone`/`clone3`, `execve`,
  `exit`/`exit_group`, `wait4`, `kill`/`tgkill`, `getpid`/`gettid`/`getppid`,
  `setpgid`/`getpgid`/`setsid`/`getsid`.
- **Signals**: `rt_sigaction`, `rt_sigprocmask`, `rt_sigreturn`,
  `sigaltstack`.
- **Sockets**: `socket`, `connect`, `accept`/`accept4`, `bind`, `listen`,
  `send*`/`recv*`, `shutdown`, `get/setsockopt`, `socketpair`.
- **Waiting/multiplexing**: `poll`/`ppoll`, `select`/`pselect6`,
  `epoll_create(1)`/`epoll_ctl`/`epoll_wait`/`epoll_pwait`, `nanosleep`,
  `clock_nanosleep`, `futex`.
- **Misc**: `clock_gettime`/`clock_getres`, `gettimeofday`, `getrandom`,
  `uname`, `prctl`/`arch_prctl`, `sched_yield`, `set_tid_address`,
  `set/get_robust_list`, `eventfd(2)`, `timerfd_create/settime/gettime`,
  `inotify_init(1)`/`inotify_add_watch`/`inotify_rm_watch`, `prlimit64`,
  `close_range`.

A handful of identity/permission syscalls are stubbed rather than fully
implemented since Tunix is currently single-user: `getuid`/`getgid`/`geteuid`/
`getegid` always return `0`, `getgroups` reports no supplementary groups.
`statx` and `rseq` are recognized but return `-ENOSYS`. Anything not listed in
the `switch` falls through to the `default` case, logs the syscall number,
and returns `-ENOSYS` (`kernel/syscall.c:3221`).

After the `switch`, `process_prepare_user_return` runs (unless the handler
explicitly opted out via `skip_signal_delivery`) to check for pending signals
before the frame is handed back to `syscall_entry` for the return to user
space.

## Process states and the process table

Every process is a `struct process` (`kernel/include/process.h:34`) kept
in one circular, singly linked list (`queue`, `kernel/process.c:45`), with
`current` pointing at whichever entry is presently running. New processes are
appended at the tail by `enqueue` (`kernel/process.c:122`).

States (`kernel/include/process.h:11`):

- `PROCESS_READY` — runnable, not currently on the CPU.
- `PROCESS_RUNNING` — the one process the CPU is executing.
- `PROCESS_BLOCKED` — waiting on I/O, a futex, or a child (`wait4`).
- `PROCESS_STOPPED` — job-control stop (`SIGSTOP`/`SIGTSTP`/`SIGTTIN`/
  `SIGTTOU`).
- `PROCESS_ZOMBIE` — exited, status not yet collected by the parent.
- `PROCESS_DEAD` — fully finished; only `process_reap_deferred`
  (`kernel/process.c:220`) still holds a reference, and it frees the
  struct, kernel stack, and (once unreferenced) address space on the next
  syscall dispatch.

Key fields on `struct process` beyond bookkeeping (pid/ppid/pgid/sid, name,
fds): `cr3` and `memory` (address space, shared and refcounted across threads
of the same `tgid`), `kernel_stack_top`, `saved_frame` (a full
`struct syscall_frame` — the process's suspended register state),
`time_slice_ticks` (remaining scheduler quantum), and the `io_wait_*`/
`futex_wait_*`/`wait_*` fields used to resume a specific blocking syscall.

## Scheduler

The scheduler is round robin over the circular `queue`, shared by every
processor — see [Multiprocessor](multiprocessor.md) for how they are started
and what keeps them out of each other's way:

- `next_runnable(after)` (`kernel/process.c:385`) walks forward from
  `after` (or from the head if `after` is `NULL`) and returns the first
  process in `PROCESS_READY` state, wrapping around the list once. A `RUNNING`
  process is not a candidate: it is loaded on some processor already.
- The quantum is `PROCESS_DEFAULT_QUANTUM_TICKS` = 5 timer ticks
  (`kernel/process.c:33`). The timer runs at `TIMER_FREQUENCY_HZ` = 250 Hz
  (`kernel/include/timer.h:8`), a PIT rate generator programmed by
  `timer_init` (`kernel/timer.c:14`), so a quantum is ~20 ms.
- `activate_process` (`kernel/process.c:397`) is the only place that makes
  a process "the" running one: it sets `current`, resets the quantum if it
  had run out, stamps `last_scheduled_ns`, sets state to `RUNNING`, points
  both kernel-stack registers (this processor's TSS `rsp0` and the
  `kernel_rsp` in its per-CPU block) at the process's kernel stack, switches
  page tables (`vmm_activate(cr3)`), and reloads `IA32_FS_BASE` for TLS.
  `current` is per-processor: it lives in the block `GS` points at.

There is no separate "context switch" assembly routine that swaps callee-
saved registers on a kernel stack the way a traditional preemptive kernel
does. Because every kernel-side path (a syscall handler, or the timer
interrupt handler) runs to completion on its own stack without ever sleeping
outside of a few defined points, a "switch" is just: copy the outgoing
process's register snapshot into its `saved_frame`, copy the incoming
process's `saved_frame` into the frame that is about to be returned to user
space via `iretq`, and call `activate_process`. This happens in
`switch_to_next` (`kernel/process.c:456`) for syscall-driven switches, and
inline in `process_timer_interrupt` for preemption.

### Preemption

`timer_irq` (`kernel/timer.c:26`) fires on every PIT tick and calls
`process_timer_interrupt` (`kernel/process.c:472`), which only acts if the
interrupt landed in user mode (`cs & 3`) on the currently running process. It
decrements `time_slice_ticks`; when it reaches zero it looks for another
runnable process via `next_runnable`. If one exists, the current process's
register snapshot is saved, `activate_process` switches to the other process,
and the *timer interrupt's own frame* is overwritten in place
(`load_interrupt_context`) so `iret` from the interrupt handler resumes the
new process instead of the old one. If no other process is runnable, the
current process just gets its quantum refilled and keeps running.

A tick that arrives on a processor with no process at all takes the same route
in reverse (`resume_from_idle`): it came from kernel mode, but long mode pushes
`SS:RSP` for those interrupts too, so the frame can be replaced with a
process's and the `iret` lands in user space. That is how an idle processor
picks up work. Processors other than the first are ticked by their own local
APIC timer rather than the PIT, which is wired to one of them.

### Voluntary and blocking transitions

- **`sched_yield`** calls `process_yield_from_syscall`
  (`kernel/process.c:503`) directly: mark self `READY`, find another
  runnable process, switch. If none exists, stay `RUNNING`.
- **Blocking I/O** (`read`, `poll`/`ppoll`, `select`/`pselect6`, `connect`,
  `recvfrom`/`recvmsg`, `nanosleep`, `clock_nanosleep`) has no wait-queue
  mechanism. Instead `retry_io_wait` (`kernel/syscall.c:605`) rewinds
  `user_rip` by 2 bytes — the length of the `syscall` instruction — and calls
  `process_yield_from_syscall`. The process is rescheduled later, re-executes
  the same `syscall` instruction from scratch, and the handler checks again
  whether the resource is ready or the deadline has passed. `syscall_dispatch`
  clears this retry state (`clear_io_wait`) if a *different* syscall number
  shows up first (e.g. the process was interrupted by a signal handler).
- **`futex(FUTEX_WAIT)`** (`process_futex_wait`, `kernel/process.c:861`)
  sets `PROCESS_BLOCKED` plus a wait address/deadline and calls
  `switch_to_next` directly (no retry-by-rip-rewind, since there is nothing
  useful to re-check without waking up first). `futex(FUTEX_WAKE)`
  (`process_futex_wake`) scans the queue for blocked waiters on the same
  address within the same address space and flips them back to `READY`;
  expired futex deadlines are swept lazily by `wake_expired_futex_waiters`,
  called from `next_runnable`.
- **`wait4`** (`process_waitpid_from_syscall`, `kernel/process.c:1000`)
  returns immediately if a matching zombie/stopped/continued child already
  exists. Otherwise it records `wait_pid`/`wait_status_user`/`wait_options` on
  the parent, sets `PROCESS_BLOCKED`, and switches away. A child's
  `notify_parent_of_exit` (`kernel/process.c:557`) writes the wait status
  and return value directly into the blocked parent's `saved_frame.rax` and
  flips it back to `READY` — the parent never re-executes the syscall, since
  it wasn't retried, it was completed on its behalf while suspended.
- **`fork`/`clone`** (`process_fork_from_syscall`,
  `process_clone_thread_from_syscall`) create a new `struct process` in
  `PROCESS_READY`, `enqueue` it, and return normally in the parent (child's
  `saved_frame.rax` is pre-set to `0`). `process_run_child_first_from_syscall`
  is used for `vfork`-style ordering: it puts the parent back in the queue as
  `READY` and immediately `activate_process`s the child instead, without
  waiting for the next scheduling point.
- **`exit`/`exit_group`** moves the process to `PROCESS_ZOMBIE` (or straight
  to `PROCESS_DEAD` for a thread that isn't the last in its `tgid`), closes
  its file descriptors, wakes/reassigns children, notifies (or completes) a
  waiting parent, and switches away via `switch_to_next`. The struct itself is
  only freed later by `process_reap_deferred`, called at the top of every
  `syscall_dispatch`.
- **Signals** are checked on every return to user space, not just at syscall
  boundaries: `process_prepare_user_return` (`kernel/process.c:1145`) runs
  both at the end of `syscall_dispatch` and at the end of
  `process_timer_interrupt`. A pending, unblocked signal can itself drive a
  state transition — `PROCESS_STOPPED` for job-control signals, or process
  exit for default-terminate signals — before the frame is handed back.

## How the pieces fit together

A syscall always enters through the same `struct syscall_frame`, whether it
arrived via `SYSCALL` (`syscall_entry`) or was reconstructed from a suspended
process's `saved_frame` by the scheduler. That shared format is what lets
`switch_to_next` and `process_timer_interrupt` treat "resume this process" as
nothing more than "point the return frame at its `saved_frame` and call
`activate_process`" — the same operation the syscall path and the timer path
both already know how to return from.
