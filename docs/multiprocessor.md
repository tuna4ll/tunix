# Multiprocessor

Tunix runs on every processor the firmware describes. This document covers how
they are started, what each of them owns privately, how the scheduler hands
work out, and what keeps them from corrupting each other's view of memory. It
reflects the code as it exists today.

## Every spin with interrupts off has to service the flush

A processor asking the others to drop cached translations sets a flag on each
of them, sends an interrupt, and waits. It does that holding the kernel lock,
because the page tables being edited are the ones they are running on.

So a processor spinning anywhere with interrupts disabled cannot be reached by
that interrupt, and has to call `smp_service_flush()` inside its own wait loop.
`wait_for_turn()` always did. The console locks, when they were added, did not
-- and what that produced on a real machine was the whole thing stopping in
the first thirty tickets of the boot:

```
KLOCK: cpu 1 stuck waiting for ticket 28: next 31 serving 27 shared 0
KLOCK: cpu 0 holds 1
```

cpu 0 holding the kernel lock, waiting for a flush from a processor that was
spinning for a lock with its interrupts off.

The wait is bounded now as well. A processor that is marked online and never
answers -- one that came up, said so and then went wrong -- used to take the
machine with it, holding the one lock everything else needs and printing
nothing. It gets two seconds, then a line saying so, and the flush is given up
on: a processor with stale translations is a worse machine than a correct one
and a much better one than a dead one.

## The lock says when it is not coming

Two ways the kernel lock can be lost for good, and both are silent from
outside. An unlock with nothing held moves the ticket queue past a ticket
nobody was serving, so every later attempt waits for a turn that has already
gone by; a shared holder that leaves without decrementing keeps every exclusive
waiter out. Either way the machine stops on somebody's next syscall having
printed nothing at all.

So a wait longer than twenty seconds says what it is waiting for, once per
processor, and goes on waiting:

```
KLOCK: cpu 3 stuck waiting for ticket 26302: next 26309 serving 26301 shared 0
KLOCK: cpu 0 holds 0 doing 20020
```

`doing` is a breadcrumb the few places that take the lock leave behind, because
there is no stack to walk from another processor: `0x1nnnn` is syscall `nnn`,
`0x2nnnn` interrupt vector `nnn`, `0x30000` the first process being started and
`0x40000` going idle. Knowing a processor is not giving the lock back is half a
diagnosis; the half that matters is what it is holding it for.

Twenty seconds rather than five because the lock is held across block reads,
and a root filesystem on a USB stick makes some of those genuinely slow --
five caught weston loading itself.

The bug it was written to find was real and was in the interrupt path. The
handler took the lock only when it was free, because an interrupt can land on
a processor already inside the kernel; the entry stub released it
unconditionally on the way out. So such an interrupt handed back a lock it
never took. `kernel_lock_from_isr()` and `kernel_unlock_from_isr()` are the
pair that agree with each other.

## The console is a shared device too

Two things write into the same screen from two directions: a terminal a program
is writing to, under the kernel lock, and the kernel log, which is not under it
at all -- kprintf() is reachable from an interrupt handler and from the fault
path, and taking the kernel lock there would be a deadlock rather than a fix.

So the terminal has a lock of its own, and kprintf() has a second one that
makes a whole message atomic. Without the first, the cell model and the cursor
get interleaved and the screen fills with coloured rubbish where a scroll got
half done. Without the second the screen is correct and unreadable: two
processors printing at once produce one line with both messages spliced into
it, a character each --

```
T UNI1: st2Iri pid 1
```

which is what a real machine printed while the trace in `verbose` was going
out unlocked from every processor at once. Both locks turn interrupts off
while held, because a processor that took one here would otherwise wait for
itself.

The terminal's lock is re-entrant per processor, and that is not a nicety: it
is held across a run of characters, and anything inside that run which prints
-- a warning from the tty layer, say -- comes back through the same lock. A
second acquisition that does not recognise its own processor spins for a lock
that processor is already holding, with interrupts off, for good.

It is held across a run of characters rather than around each one:
per character it was correct and far too expensive -- interrupts off, a
contended cache line and a released lock for every glyph -- and what it
produced was a processor holding the *kernel* lock for seconds at a stretch
while it painted. `tty_write()` takes it once for the whole write, which is
also what stops a kernel message landing in the middle of a program's line.

panic() takes both back by force before it prints: the processor holding one
may be the one that just went wrong.

## Finding the processors

`acpi_describe_machine` (`kernel/drivers/acpi.c`) walks the MADT. A type 0 entry is
a processor: it carries an ACPI id, a local APIC id, and flags saying whether
the socket is filled. The APIC id is what a startup message is addressed to and
it is **not** the index — firmware numbers processors however it likes, and a
machine with hyperthreading disabled in its BIOS leaves gaps.

`SMP_MAX_CPUS` (8, `kernel/include/percpu.h`) is the ceiling. A table that
lists more says so on the console rather than silently rounding down.

## Bringing one up

`smp_init` (`kernel/smp.c`) runs at the end of `kmain`, after the APIC, the
timer and the syscall MSRs, and before the first process starts. For each
processor other than the one running:

1. The parameter block in the trampoline is filled in — page tables, stack,
   entry point, index.
2. An INIT message resets it; ten milliseconds later a startup message names
   the page it begins executing at. The startup is sent twice, as the manual
   asks.
3. The starter waits up to 200 ms for the new processor to say it is up.

The processors are started one at a time: there is one parameter block, and a
failure is easier to attribute that way.

### The trampoline

`kernel/arch/x86_64/trampoline.S` is copied to physical `0x8000` and runs
from there, so nothing in it may use a link-time address; every reference is
written as an offset into the blob plus the address it was copied to.

A processor woken this way starts in 16-bit real mode with nothing set up. The
blob reaches 32-bit protected mode, turns on PAE, loads the kernel's own page
tables, sets `EFER.LME` and jumps to 64-bit code, which loads the stack it was
given and calls `smp_ap_entry`.

Two details are load-bearing:

- **NXE before paging.** The kernel's page tables already have bit 63 set on
  every non-executable page. To a processor with `EFER.NXE` clear that bit is
  reserved, so the first instruction fetch after paging comes on would fault
  rather than run.
- **The trampoline's page must be mapped to itself.** The instruction after the
  one that enables paging has to be fetched through the tables it just
  installed, and the kernel maps nothing that low. The loader's identity map
  normally survives in the tables the kernel inherited, so usually there is
  nothing to do — and because it is a huge page, mapping over it would fail
  rather than be redundant. `map_trampoline_page` checks first and only adds a
  mapping when one is missing.

Until `smp_ap_entry` loads an IDT there is no handler for anything, so a
mistake in here is a triple fault and a silent reboot, not a message.

## What each processor owns

`struct cpu` (`kernel/include/percpu.h`) is reached through `GS`. That is
the only way a piece of kernel code can find out which processor is running it:
every other name in the kernel is shared.

`GS` holds the block only in kernel mode. The entry and exit stubs `swapgs`,
and the user side of the pair is whatever the process had. Two rules follow
from the fact that loading a segment register clears its base:

- `gdt_init_cpu` sets the base **after** `gdt_flush`, not before.
- `process_enter_user` swaps **before** it loads the user data segments.

In `isr_common_stub` the swap is decided by the privilege each end of the
interrupt came from and is going to, which are not always the same frame: a
tick taken in the idle loop arrives at CPL 0 with the block already in `GS` and
leaves through a process's frame at CPL 3, so it swaps on the way out and not
on the way in.

Private per processor: the GDT and TSS (`rsp0` is the kernel stack of whichever
process *this* processor is running, and `ltr` marks its own descriptor busy),
the running process, the address space loaded, the idle stack, and the local
APIC timer. Shared: the IDT (same handlers everywhere, one IDTR each), the page
tables, and every kernel data structure.

## The scheduler

The process queue is global and any processor may take anything off it. Most of
what that took is one line:

```c
static int runnable(const struct process *process) {
    return process && process->state == PROCESS_READY &&
           allowed_on_this_cpu(process);
}
```

`RUNNING` used to be pickable, and on one processor that was harmless. On
several it means "a processor has this loaded right now": picking it again
elsewhere would run the same registers twice and let two return paths write the
same saved frame. Every path that gives a process up already marks it `READY`
(or blocked, or dead) before it looks for the next one, so nothing is lost.

The other predicate is task affinity. The default mask includes every online
processor; `sched_setaffinity` can narrow it, and fork/clone inherit it. If a
task removes its current processor from the mask, it yields immediately. If
another processor changes the mask of a running task, that task notices on its
next timer tick and migrates through the ordinary READY/idle path.

### Idling

A processor with nothing to run parks. `go_idle` drops the process it was
holding, returns to the kernel address space — so the process's own can be torn
down — and jumps to the idle stack, where it sits in `sti; hlt`. The kernel
stack it was on is abandoned rather than unwound, for the same reason a
blocking syscall can switch away mid-call: everything worth keeping is already
written down in the process.

It comes back through its timer. In long mode the processor pushes `SS:RSP` for
interrupts taken at CPL 0 as well, so the frame can be replaced wholesale with
a process's and the `iretq` lands in user mode — which is how an idle processor
picks up work with no context to unwind (`resume_from_idle`).

The first processor is driven by the PIT, as it always was. Every other one is
preempted by its own local APIC timer, whose rate nothing reports and so is
measured against the TSC at bring-up.

The latency for an idle processor to notice new work is therefore up to one
tick, 4 ms. There is no reschedule message, because the case it would cover —
more runnable processes than busy processors — is the case where throughput is
not the constraint. A processor that blocks picks the next runnable process
itself, immediately, exactly as before.

## The kernel lock

Nothing in this kernel was written to be entered twice at once: the process
queue is a bare linked list, the VFS tree has no locks, the page tables are
edited in place. So kernel entry is serialised behind a single ticket lock
(`kernel/klock.c`), taken in `syscall_dispatch` and `isr_handler` and
dropped by the assembly that called them.

This does not cost the parallelism that was wanted: user code is where the time
goes and user code does not hold it. Kernel mode runs with interrupts off, so a
processor holding the lock cannot be interrupted into wanting it again, and the
wait is always finite. A ticket lock rather than a test-and-set so that a
processor entering the kernel in a tight syscall loop cannot starve one that has
been waiting.

One consequence shows up in `isr_dispatch`: interrupts are acknowledged before
they are handled, not after. A tick that ends up parking the processor — the
last process on it exited — never returns to the handler, and a controller
still waiting to be told the last interrupt finished will not send another.

### The frame the return path is standing on

There is a second consequence, and it is the subtlest thing here.

A frame arrives on the kernel stack of the process that made the call. If that
call blocked, the process gave its processor away and the frame the return path
now holds belongs to somebody else — while the stack under it belongs to a
process that another processor is free to schedule the instant the lock is
dropped. The first thing that process does on entering the kernel is write over
exactly those bytes. The same is true after an exit, where the stack can be
freed and handed back to the heap.

Reasoning that the window is only twenty instructions wide does not save it: a
guest processor can be descheduled by its host between any two of them, for
milliseconds.

So `syscall_dispatch` and `isr_handler` move the frame onto the kernel stack of
whichever process is running here *now* — a stack no other processor can enter
while this one holds the process — and the assembly reloads `rsp` from the same
per-CPU field. When nothing was switched, source and destination are the same
address and no copy happens. Only frames returning to user mode need it; one
going back to the idle loop is on a stack of this processor's own already.

Moving the frame is not enough on its own, and the first version of this got it
wrong. A C function's *own* return address is also on the stack it is trying to
get off, so neither of those two may drop the lock: the `ret` that follows
would read a stack that another processor is free to reap and hand back to the
allocator by then — and since the heap gives pages back to the physical
allocator, that read can fault outright. Both return with the lock still held,
and the assembly drops it after `rsp` has moved. That is also why the
translation-flush interrupt has a stub of its own rather than a case in the
common one: it must not go near the lock at all.

## Cached translations

Changing a mapping on one processor does not change what another has cached.
The kernel reaches user memory by walking the page tables in software
(`vmm_copy_from_space`), so only *user* accesses are exposed — but that is
enough: a processor still holding a translation to a page that has just been
freed would keep writing into memory handed to somebody else.

`smp_flush_address_space` (`kernel/smp.c`) is called wherever a mapping is
removed or narrowed: `vmm_unmap_page_in`, `vmm_protect_page_in`, the copy path
of `vmm_handle_cow_fault` (before the frame goes back to the allocator), and
`vmm_clone_address_space`, which clears write permission on the *parent*'s
pages. It marks each processor that has this address space loaded, sends one
message, and waits for all of them to answer.

The waiting is what makes it safe, and it is also where a naive implementation
deadlocks: a processor waiting for the kernel lock has interrupts off and can
never take the message. So there are two places a request is answered — the
interrupt stub, which never touches the lock, and `kernel_lock`'s wait loop,
which checks the flag on every spin. Between them, every state a processor can
be in is covered:

| Where it is | How it answers |
| --- | --- |
| User mode | Takes the interrupt straight away |
| Idle loop | Takes the interrupt straight away |
| Waiting for the kernel lock | Answers in the wait loop |
| About to `iretq` back to user | Interrupt fires as `IF` comes back on |

It cannot be holding the lock, because the processor asking is.

Which processors are looking at a space cannot change underneath the asker:
only a processor inside the kernel changes its own, and the asker holds the
lock. A process whose address space is on one processor only — every
single-threaded program — costs nothing but a loop over eight slots.

### Faults that are no longer errors

Two page-fault paths used to treat "already mapped" as proof that the fault was
something else. With threads on several processors it is the ordinary race, so
`process_commit_area` and `process_grow_user_stack` now report it as handled
and let the instruction be retried, and `vmm_handle_cow_fault` returns success
for a page that is already private and writable.

## A thread you are not allowed to kill

`exit_group` takes the whole thread group down, and it used to do that by
walking the queue and marking every sibling dead on the spot — closing its
files and letting it be reaped. On one processor that was safe, because a
sibling was always ready or blocked and never actually executing.

On several it is not. A sibling can be `PROCESS_RUNNING` on another processor,
in the middle of its own user code, and taking its files and address space away
underneath it turns that processor's next page fault into a kernel panic
instead of a signal — the fault handler finds a `current` that is not running
and has nothing sensible left to do.

So `terminate_sibling_threads` marks a running sibling `group_exit_pending` and
leaves it alone. `process_prepare_user_return` reads that flag on the way back
to user mode and runs the ordinary exit path there, on the processor that owns
the thread. The delay is at most one tick.

This is the shape of the whole problem, and it is worth stating plainly: on one
processor, "not currently scheduled" and "not currently executing" are the same
sentence. On several they are not, and every place the old kernel relied on
that is a place to look.

## A set that is missing whatever is running

The same sentence has a second edge, and it cost a machine.

The scheduler places a waking task at the lowest virtual runtime anything
runnable holds, so that a task which slept does not come back owed the whole
time it was asleep. The obvious place to take that floor from is the set
`next_runnable()` already walks — everything `READY`. It is the wrong set. A
task holding the lowest virtual runtime is not `READY`, it is `RUNNING`,
because being lowest is why it was chosen; the `READY` set is what was *just
preempted*, which is the highest.

On one processor that hides one task and the floor is close enough to right. On
four it hides four, and the floor climbs past the running set entirely — so a
waking task gets placed *above* the tasks it is competing with and is never
chosen again. The first version of that patch did exactly this, and what it
produced was a four-processor machine that stopped dead in the wake-latency
test while the one-processor machine passed it.

The floor is taken over `RUNNING` as well as `READY` now, and over every
processor rather than the ones the asking task is allowed on. "Runnable" and
"in the runnable set" are the same phrase on one processor and different
answers on four.

## "Nothing else to run" is now a different sentence too

Several blocking paths ended with the same shape:

```c
if (switch_to_next(frame, waiting) != 0) {
    waiting->state = PROCESS_RUNNING;   /* carry on instead */
    return -SOMETHING;
}
```

On one processor that was a reasonable last resort: if the scheduler could find
nothing else, blocking would have stopped the machine, so the caller was left
running and told to try again.

On several processors the premise is gone. `next_runnable` deliberately refuses
a process that is `RUNNING`, because a `RUNNING` process is loaded on another
processor — so "nothing else to run" now routinely means "everything else is
already running somewhere", which is the opposite of an idle machine.

For `wait4` that turned into a wrong answer rather than a slow one. It returned
`ECHILD` — *you have no such child* — about a child that was executing on
another processor at that exact moment. Bash believes it: it reads `ECHILD` as
"the job is finished" and runs the next command on top of one that is still
going. The symptom was a test binary that printed nothing and an `rcS` that
sailed past it, with no fault, no signal and no message anywhere. The trace that
found it showed the shell's next command interleaved with output from the
program it was supposed to be waiting for.

`wait4` and the job-control stop now park the processor instead. The other two
(`futex(FUTEX_WAIT)` and `process_sleep_on`) keep the retry, and deliberately:
their callers rewound the syscall before sleeping, so retrying re-tests the
condition and is a correct — if wasteful — answer, where parking would turn a
wakeup that never came into a hang instead of a slow loop.

## When the machine just resets

Not everything four processors find is a concurrency bug. An Xfce session run
as an unprivileged user — from the ports build that no longer exists — reset the
machine outright: no panic, no message, QEMU simply pausing on the guest's
reset.

That shape is a triple fault: the processor could not deliver a fault, could not
deliver the double fault that followed, and gave up. Nothing is printed because
nothing gets to run. The fix for *seeing* it is an IST stack: vector 8 is given
a stack of its own in `gdt.c`, so a double fault is delivered on memory the
original failure cannot have broken, and `isr_handler` reports it before taking
the kernel lock — the processor may well have been holding it.

The first one it caught said:

```
DOUBLE FAULT: rip 0xffffffff80101645 cs 8 rsp 0xffffff0000000000
              cr2 0xfffffefffffffff8 rflags 10406
```

`rsp` is exactly `HEAP_VIRTUAL_BASE` and `cr2` is eight bytes below it: the
stack pointer had ended up at the very bottom of the heap, and the next push
left the mapped region. The instruction is the `call kernel_lock` at the top of
`isr_handler` — not a deep frame, just the first push after an interrupt landed
somewhere with nothing under it.

Growing the report a line at a time — each line reading one thing further from
the processor, so that where it stops is itself an answer — got to this:

```
  gs 0x0 kernelgs 0xffffffff8017e3e0
  cpu 0 kernel_rsp 0xffffff00002ba7b0 current 0xffffff00002b1ad0
  pid 2824936 stack 0xffffff00002b27b0..0xffffffff80100bb4
```

The process is corrupt. `kernel_stack_base` and `kernel_rsp` agree with each
other exactly (0x8000 apart, as they should be) but the pid is nonsense and
`kernel_stack_top` holds a *kernel text address*. Something had written over the
middle of a `struct process`.

The cause is in the flags. Every one of these reports had `rflags` with bit 10
set — the direction flag:

```
rflags 10406   10446   10497   10482
```

The System V ABI requires DF to be clear on entry to a C function, and the
compiler acts on that: a struct assignment or a `memset` becomes `rep movs` or
`rep stos`, which walks *backwards* when DF is set and writes over whatever
lies before the destination instead of after it. User code sets DF legitimately
— glibc's `memmove` does, for an overlapping copy — and an interrupt landing in
that window handed the flag straight to the kernel.

The syscall path never had the problem: `FMASK` clears DF on the way in, which
is why this only ever arrived through interrupts. The interrupt stubs now do the
same thing with a `cld`, which is the whole fix.

Two other things changed in the same hunt and are worth keeping, though neither
was the cause:

- **Epoll nesting is bounded now.** `file_poll_events` asks an epoll set whether
  it is ready, which asks the same of every descriptor in it — and a descriptor
  can be another set. Nothing stopped them forming a ring, and a ring has no
  answer. Linux caps the nesting at five; Tunix capped it at nothing.
- **Kernel stacks are 32 KiB rather than 16.** The deepest ordinary path through
  `syscall_dispatch` measures 9688 bytes, so 16 was survivable — but `sys_read`
  (4120) calling a `/proc` reader (4112) is 8 KiB before the VFS frames between
  them, and a panel applet reading `/proc` continuously is what found it. That
  is closer than a stack whose overflow resets the machine should ever be.

None of this was caused by more processors. It was reachable all along; four
processors and a desktop are what got somebody looking.

## Asking the machine what it is

Everything above was measured on an emulator, and the emulator is the one
machine that cannot disagree with the assumptions underneath it: its counter is
synthesised from one clock, every processor's timer runs at the same rate, and
the firmware tables say what QEMU decided to say. Real hardware is where those
stop being free.

`hwreport` on the kernel command line -- or the boot menu entry of that name --
prints what this machine turned out to be and writes the same text to
`/tunix-hwreport.txt` on the root filesystem. The file is the point: a machine
with no serial cable can still be asked, because the disk can be read anywhere
else afterwards.

```
tunix hardware report

processor
  name        AMD Ryzen 5 3600 6-Core Processor
  vendor      AuthenticAMD family 23 model 1 stepping 0
  cmdline     root=LABEL=tunix-root hwreport
clock
  tsc_hz      3600606282
  invariant   NO -- the counter may stop or change rate
processors
  firmware    4 described, 4 running
  madt[0]     acpi_id 0 apic_id 0 usable
  cpu 0       apic_id 0 timer PIT clock_skew_ns 0
  cpu 1       apic_id 1 lapic_hz 62519800 count 250079 clock_skew_ns 0
memory
  usable_mib  4084
```

The benchmarks go the same way. `make testimage TEST=schedbench` builds the
image without booting it -- `IMAGE_TABLE=mbr` for an old BIOS booting from a
stick -- and the program writes everything it prints to
`/tunix-<test>-results.txt` as well as to the console, so a run on real hardware
can be read afterwards without a cable. `TEST` is `schedbench`, `perftest` or
`drmtest`.

Three of the report's lines are there because of a specific way a real machine
can go wrong and an emulated one cannot:

- **`invariant`**, because a counter that stops in a sleep state or changes rate
  is not a clock two processors can subtract readings of, and a virtual runtime,
  a deadline and a slice all assume they can.
- **`clock_skew_ns`**, which is how far a processor's own reading of the clock
  fell outside the window the starter bracketed it with. Anything but zero means
  the processors do not share a counter.
- **`count`**, the local timer divisor each processor measured for itself. One
  that calibrated wrong gets a count of 1, which is millions of interrupts a
  second on that processor and a machine that stops; the report says so rather
  than leaving it to be guessed at.

## Proving it

A program that times one child doing a fixed amount of arithmetic, then four
children doing that much each at once. The ratio between "what four would have
cost one after another" and what they actually cost is a number a fast context
switch cannot fake. It was `bin/smp-test`, built against the kernel's own libc;
that libc is gone and so is the program, but the measurement it produced is
what the numbers below are.

`support/tests/schedbench.c` is what measures it now, and `make schedbench` is how.
It is freestanding rather than built against a libc, so it cannot go the same
way, and its root filesystem is one static binary — no Void download, no
filesystem that has to hold ownership, a few seconds to build and boot. It runs
under KVM where there is one, because the costs that separate one processor
from four are the ones an emulator does not have.

On `-smp 4` (four consecutive runs gave 338, 297, 362 and 327 percent):

```
SMPTEST: one worker 262 ms
SMPTEST: 4 workers 310 ms
SMPTEST: speedup 338 percent
SMPTEST: PASS work ran on more than one cpu
```

The same binary on the same image with `-smp 1`, which is what makes the number
above mean anything:

```
SMPTEST: one worker 280 ms
SMPTEST: 4 workers 1060 ms
SMPTEST: speedup 105 percent
SMPTEST: FAIL work was serialised
```

Two and three processors land where they should: `-smp 2` gives 212 percent,
and `-smp 3` gives 208 — four workers over three processors is two rounds, not
one and a third. The spread between runs is the host showing through; the
measurement is wall clock and the guest does not own the machine.

`/proc/cpuinfo` carries one stanza per running processor. `sched_getaffinity`
answers with the mask the thread actually has, which matters more than it
looks: `nproc` and every thread pool that sizes itself ask that first and only
fall back to `/proc/cpuinfo`, so a kernel that returns `ENOSYS` there reports
one processor however many it is running. `sched_setaffinity` narrows the mask
for real — see the scheduler section above, and `allowed_on_this_cpu()` in
`kernel/process.c`.

The other proof was a desktop: a full Xfce session — Xorg, xfwm4, xfce4-panel,
xfdesktop, Thunar, all of it heavily threaded — coming up and staying up on four
processors is what turned each of the bugs above from a theory into a log. That
session was built by the ports tree and went with it; the bugs it found did not,
and the userland that runs now is Void's threaded one.

`make run` and `make headless` pass `-smp 4`. `QEMU_SMP=1` runs the machine as
it was before there was more than one.
