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
handlers are defined in `core/syscall.c:46` (the `SYS_*` `#define`
block).

## Entry path

`syscall_init` (`core/syscall.c:527`) programs the syscall MSRs once at
boot:

- `EFER.SCE` (MSR `0xC0000080`) is set to enable the `SYSCALL`/`SYSRET`
  instructions, plus the NX bit if the CPU supports it.
- `STAR` (MSR `0xC0000081`) sets the kernel/user code segment selectors used
  on entry/exit.
- `LSTAR` (MSR `0xC0000082`) points at `syscall_entry`, the raw entry point.
- `FMASK` (MSR `0xC0000084`) clears `IF` and `DF` on entry.

`syscall_entry` (`arch/x86_64/syscall_entry.S:20`) is hand-written
assembly, not a C function, because it runs before there is a valid kernel
stack:

1. It `swapgs`es to bring this processor's own block into reach, stashes the
   incoming user `rsp` there, and switches to the kernel stack the block
   names (set by `syscall_set_kernel_stack` — see below). A global would not
   do: which stack to switch to is a different answer on every processor, and
   the question has to be answered before there is a stack to answer it with.
2. It spills all argument/callee-relevant registers plus `rcx`/`r11`
   (the `SYSCALL`-clobbered return `rip`/`rflags`) and the saved user `rsp`
   into a `struct syscall_frame` (`include/tunix/syscall.h:6`) on that
   stack.
3. It calls `syscall_dispatch(frame)`.
4. On return, it rebuilds an `iretq` frame from the (possibly modified)
   `syscall_frame`, `swapgs`es back, and returns to user space via `iretq`
   rather than `sysretq`.

Using `iretq` for every return — even the ordinary `SYSCALL` fast path — is
deliberate: it lets the scheduler resume a process with one common frame
format regardless of whether it was last suspended by a syscall or by the
timer interrupt (see below). `process_enter_user`
(`arch/x86_64/syscall_entry.S:87`) uses the same `iretq` technique
for the very first entry into user space.

Two separate "kernel stack" pointers exist and both are updated together
whenever the scheduler switches processes (`activate_process`,
`core/process.c:397`):

- `tss.rsp0`, set via `set_kernel_stack` (`arch/x86_64/gdt.c`) —
  used by the CPU for privilege-level changes on interrupts/exceptions
  (e.g. the timer IRQ).
- `kernel_rsp` in the per-CPU block, set via `syscall_set_kernel_stack` —
  used explicitly by `syscall_entry` because `SYSCALL` does not consult the
  TSS.

Both are per-processor, and for the same reason: they name the kernel stack of
whichever process *this* processor is running.

## Dispatch table

`syscall_dispatch` notes the entry, takes no lock, and calls `syscall_run`
(`core/syscall.c`), a single `switch` on `frame->rax`. On every call it
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
  `sigaltstack`, `rt_sigpending`, `rt_sigsuspend`, `rt_sigtimedwait`,
  `rt_sigqueueinfo`/`rt_tgsigqueueinfo`, `signalfd(4)`.
- **Sockets**: `socket`, `connect`, `accept`/`accept4`, `bind`, `listen`,
  `send*`/`recv*`, `shutdown`, `get/setsockopt`, `socketpair`.
- **Waiting/multiplexing**: `poll`/`ppoll`, `select`/`pselect6`,
  `epoll_create(1)`/`epoll_ctl`/`epoll_wait`/`epoll_pwait`, `nanosleep`,
  `clock_nanosleep`, `futex`.
- **Timers**: `timer_create`/`timer_settime`/`timer_gettime`/
  `timer_getoverrun`/`timer_delete`, `setitimer`/`getitimer`, `alarm`.
- **Misc**: `clock_gettime`/`clock_getres` (including the process and thread
  CPU-time clocks), `gettimeofday`, `getrandom`,
  `uname`, `prctl`/`arch_prctl`, `sched_yield`, `set_tid_address`,
  `set/get_robust_list`, `eventfd(2)`, `timerfd_create/settime/gettime`,
  `inotify_init(1)`/`inotify_add_watch`/`inotify_rm_watch`, `prlimit64`,
  `close_range`.

A handful of identity/permission syscalls are stubbed rather than fully
implemented since Tunix is currently single-user: `getuid`/`getgid`/`geteuid`/
`getegid` always return `0`, `getgroups` reports no supplementary groups.
`statx` and `rseq` are recognized but return `-ENOSYS`. Anything not listed in
the `switch` falls through to the `default` case, logs the syscall number,
and returns `-ENOSYS` (`core/syscall.c:3221`).

After the `switch`, `process_prepare_user_return` runs (unless the handler
explicitly opted out via `skip_signal_delivery`) to check for pending signals
before the frame is handed back to `syscall_entry` for the return to user
space.

## Process states and the process table

Every process is a `struct process` (`include/tunix/process.h:34`) kept
in one circular, singly linked list (`queue`, `core/process.c:45`), with
`current` pointing at whichever entry is presently running. New processes are
appended at the tail by `enqueue` (`core/process.c:122`).

States (`include/tunix/process.h:11`):

- `PROCESS_READY` — runnable, not currently on the CPU.
- `PROCESS_RUNNING` — the one process the CPU is executing.
- `PROCESS_BLOCKED` — waiting on I/O, a futex, or a child (`wait4`).
- `PROCESS_STOPPED` — job-control stop (`SIGSTOP`/`SIGTSTP`/`SIGTTIN`/
  `SIGTTOU`).
- `PROCESS_ZOMBIE` — exited, status not yet collected by the parent.
- `PROCESS_DEAD` — fully finished; only `process_reap_deferred`
  (`core/process.c:220`) still holds a reference, and it frees the
  struct, kernel stack, and (once unreferenced) address space on the next
  syscall dispatch.

Key fields on `struct process` beyond bookkeeping (pid/ppid/pgid/sid, name,
fds): `cr3` and `memory` (address space, shared and refcounted across threads
of the same `tgid`), `kernel_stack_top`, `saved_frame` (a full
`struct syscall_frame` — the process's suspended register state),
`time_slice_ticks` (remaining scheduler quantum), and the `io_wait_*`/
`futex_wait_*`/`wait_*` fields used to resume a specific blocking syscall.

## Scheduler

The scheduler scans the circular `queue`, shared by every processor — see
[Multiprocessor](multiprocessor.md) for how they are started and what keeps
them out of each other's way:

- `next_runnable(after)` first finds the highest `rt_priority` on this CPU's
  runnable set. Real-time equals remain round robin. In the ordinary band it
  selects the lowest `virtual_runtime_ns`, with ties resolved by queue order.
  A `RUNNING` process is not a candidate: it is loaded on some processor
  already. A process whose affinity mask excludes this CPU is not a candidate
  either.
- `rt_priority` is 0 for everything unless a thread asks, through
  `sched_setscheduler(SCHED_FIFO|SCHED_RR)`, for 1 to 99. Higher runs first and
  equals take turns. `SCHED_FIFO` is accepted and then scheduled as `SCHED_RR`:
  running a thread until it blocks, as FIFO promises, lets one loop stop the
  machine with no way back in, and the difference only shows between two
  runnable threads at one priority that never sleep.
- A tick also hands the processor over when something with a higher priority is
  waiting, rather than only when the quantum runs out. Without that a priority
  is worth a great deal less: the waiting thread would sit through up to a
  whole 20 ms quantum. Measured, on a machine with six spinners and four
  processors, as how late a thread asking to wake every 20 ms actually woke:

  ```
  ordinary priority            median 8.4 ms, worst 43.0 ms
  SCHED_RR 10, no preemption   median 11.5 ms, worst 23.5 ms
  SCHED_RR 10, with it         median 4.5 ms, worst 23.6 ms
  ```

  What asks for this is the sound mixer; see [Sound](sound.md), where the same
  change took a game from 322 underruns to none.
- `nice` selects one of 40 geometric weights. Runtime advances virtual runtime
  by `elapsed * 1024 / weight`, so a lower-weight task becomes less eligible
  sooner. Forked processes and threads start at the parent's virtual runtime;
  they cannot gain an initial zero-runtime advantage.
- A task that wakes is *placed* rather than left where it was. Virtual runtime
  stands still while a task is blocked and rises for everything that runs, so
  without this a task that slept for two seconds came back two seconds of credit
  ahead and then held the processor until it had spent all of it — measured at
  1500 ms of a spinner not being scheduled once. `place_waking_task`
  (`core/process.c`) puts it at the runnable set's own virtual runtime less
  half the target latency: the subtraction is what keeps an interactive task
  prompt, the floor is what stops the debt being unbounded.
- That floor is taken over `RUNNING` tasks as well as `READY` ones. The task
  holding the lowest virtual runtime is the one running, so a floor drawn from
  the ready set alone is drawn from what was just preempted — and on four
  processors that is four tasks hidden and a floor that climbs past all of them.
  See [Multiprocessor](multiprocessor.md).
- A single accounting sample longer than a second is dropped. A slice is never
  that long, so such a sample subtracted two clocks that do not agree, and
  charging it would put the task behind everything for ever.
- Ordinary quanta are proportional to weight within a 24 ms target latency,
  with a one-tick (4 ms) minimum granularity. When the runnable population is
  larger than the six-tick target, the period expands so every task can receive
  at least one tick. Real-time tasks retain their five-tick quantum.
- On each tick, an ordinary task is also preempted if another runnable task is
  over one tick behind in virtual runtime.
- Waking a task does not wait for that tick. If no processor is idle, the
  waker picks the processor whose task the woken one would preempt (a lower
  real-time priority, or more than one tick ahead in virtual runtime) and sends
  that processor alone a reschedule interrupt, which runs the same check at
  once. An interactive task that slept while CPU-bound work ran gets a
  processor within microseconds.
- `sched_setaffinity` stores a non-empty mask limited to online CPUs;
  `sched_getaffinity` reports the selected thread's effective mask. Masks are
  inherited by fork and clone, and a task excluded from its current CPU is
  migrated at the next scheduling point.
- `activate_process` (`core/process.c`) is the only place that makes
  a process "the" running one: it sets `current`, resets the quantum if it
  had run out, stamps `last_scheduled_ns`, sets state to `RUNNING`, points
  both kernel-stack registers (this processor's TSS `rsp0` and the
  `kernel_rsp` in its per-CPU block) at the process's kernel stack, switches
  page tables, and reloads `IA32_FS_BASE` for TLS. `current` is per-processor:
  it lives in the block `GS` points at.

  FPU and SIMD registers are saved and loaded only when the process actually
  changes. Re-activating the process that is already running, as the
  interrupt path does when a signal is pending, leaves them alone: they are
  live, and the copy in memory is from the last time it was switched out.

  A switch made from inside a syscall or an interrupt hands the next process
  the frame it is returning through, so it goes back to user space on the
  kernel stack of the process that gave up the processor. That is only safe
  while the return is short. When the next process has a signal to deliver or
  a group exit to carry out, it is started on its own kernel stack instead
  (`abandon_to`), because that work can take mutexes and sleep, and a process
  must never sleep on a stack that belongs to someone else.

  `vmm_activate(cr3)` runs only when the incoming process's address space is
  not the one already loaded. Writing CR3 discards every translation the
  processor had cached, and two threads of one process share a `cr3`, so
  switching between them used to flush the TLB for nothing. Measured, a
  ping-pong between two threads of one process and the same between two
  processes cost 2606 ns each before and 1848 ns against 2575 ns after.

### Preemption

`timer_irq` (`time/timer.c:26`) fires on every PIT tick and calls
`process_timer_interrupt` (`core/process.c:472`), which only acts if the
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
timer rather than the PIT, which is wired to one of them; the same timer also
fires at the earliest sleeper's deadline (see
[Multiprocessor](multiprocessor.md#idling)).

### Voluntary and blocking transitions

- **`sched_yield`** calls `process_yield_from_syscall`
  (`core/process.c:503`) directly: mark self `READY`, find another
  runnable process, switch. If none exists, stay `RUNNING`.
- **Blocking I/O** (`read`, `poll`/`ppoll`, `select`/`pselect6`, `connect`,
  `recvfrom`/`recvmsg`, `nanosleep`, `clock_nanosleep`) has no wait-queue
  mechanism. Instead `retry_io_wait` (`core/syscall.c:605`) rewinds
  `user_rip` by 2 bytes — the length of the `syscall` instruction — and calls
  `process_yield_from_syscall`. The process is rescheduled later, re-executes
  the same `syscall` instruction from scratch, and the handler checks again
  whether the resource is ready or the deadline has passed. `syscall_dispatch`
  clears this retry state (`clear_io_wait`) if a *different* syscall number
  shows up first (e.g. the process was interrupted by a signal handler).
- **`futex(FUTEX_WAIT)`** (`process_futex_wait`, `core/process.c:861`)
  sets `PROCESS_BLOCKED` plus a wait address/deadline and calls
  `switch_to_next` directly (no retry-by-rip-rewind, since there is nothing
  useful to re-check without waking up first). `futex(FUTEX_WAKE)`
  (`process_futex_wake`) scans the queue for blocked waiters on the same
  address within the same address space and flips them back to `READY`;
  expired futex deadlines are swept lazily by `wake_expired_futex_waiters`,
  called from `next_runnable`.
- **`wait4`** (`process_waitpid_from_syscall`, `core/process.c:1000`)
  returns immediately if a matching zombie/stopped/continued child already
  exists. Otherwise it records `wait_pid`/`wait_status_user`/`wait_options` on
  the parent, sets `PROCESS_BLOCKED`, and switches away. A child's
  `notify_parent_of_exit` (`core/process.c:557`) writes the wait status
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
  boundaries: `process_prepare_user_return` (`core/process.c:1145`) runs
  both at the end of `syscall_dispatch` and at the end of
  `process_timer_interrupt`. A pending, unblocked signal can itself drive a
  state transition — `PROCESS_STOPPED` for job-control signals, or process
  exit for default-terminate signals — before the frame is handed back.

## Signal queues

Every pending signal carries the `siginfo` it was sent with, in a queue per
thread (`signal_queue` in `struct process`). A standard signal is pending at most
once: a second `kill` while the first waits changes nothing, as on Linux. A
real-time signal (32 to 64) queues every instance, each with its own sender and
value, up to `RLIMIT_SIGPENDING`, which starts at one entry per 64 pages of
memory as Linux's does; past it `sigqueue` fails with `EAGAIN`.

What fills the record:

- `kill`, `tkill`, `tgkill` and `sigqueue` set `SI_USER`, `SI_TKILL` or the
  caller's code, with its pid and real uid. A caller may only pick a code of its
  own (`SI_QUEUE` and below) when it signals another process.
- A fault sets `si_addr` and `SEGV_MAPERR`/`SEGV_ACCERR` (or the `SIGBUS`,
  `SIGILL`, `SIGFPE` and `SIGTRAP` codes), and the `ucontext` gets the same
  address (`trapno`, `err` and `cr2` on x86_64, `fault_address` on arm64). A fault on a blocked or ignored signal
  resets it to the default action, so the process dies instead of faulting
  forever.
- A child's exit, stop and continue send `SIGCHLD` with `CLD_EXITED`,
  `CLD_KILLED`, `CLD_STOPPED` or `CLD_CONTINUED`, its pid and status;
  `SA_NOCLDSTOP` drops the last two.
- A POSIX timer queues one preallocated entry with `SI_TIMER`; when it fires
  again before that entry was taken, the expiry is counted as an overrun instead
  (`si_overrun`, `timer_getoverrun`).

A signal that is ignored and not blocked is dropped when it is sent, and
setting a signal to be ignored drops what is pending. Delivery takes the
synchronous fault signals first and then the lowest number, and a signal still
pending when a handler returns is delivered on the way out of `rt_sigreturn`.
Process-directed signals go to the thread group leader; stop, continue and kill
reach every thread of the group. Delivery copies the action once, under the
scheduler lock, because another thread's `sigaction` may change it meanwhile; an
action that became `SIG_IGN` drops the signal rather than jump to address 1.

The handler gets the interrupted code's FPU and vector state above its frame, in
Linux's layout (an XSAVE image with `FP_XSTATE_MAGIC1`/`2` on x86_64, an
`fpsimd_context` record on arm64), and `rt_sigreturn` loads it back after
clearing what would fault the restore. `rt_sigreturn` keeps the arithmetic
flags from the saved `eflags` -- they are the interrupted code's condition
codes, and dropping them sent it down the wrong branch at random.

## How the pieces fit together

A syscall always enters through the same `struct syscall_frame`, whether it
arrived via `SYSCALL` (`syscall_entry`) or was reconstructed from a suspended
process's `saved_frame` by the scheduler. That shared format is what lets
`switch_to_next` and `process_timer_interrupt` treat "resume this process" as
nothing more than "point the return frame at its `saved_frame` and call
`activate_process`" — the same operation the syscall path and the timer path
both already know how to return from.
