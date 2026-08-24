#ifndef TUNIX_OPLOCK_H
#define TUNIX_OPLOCK_H

/*
 * The locks a shared-mode path takes, and the rule that says which.
 *
 * The whole of this rests on one property of the kernel lock: an exclusive
 * holder excludes every shared holder, and a shared holder cannot start while
 * an exclusive one is inside. So a path running in shared mode is running
 * alongside *other shared paths and nothing else*. It does not have to be safe
 * against fork, or close, or the scheduler, or an interrupt -- all of those are
 * exclusive and are therefore not happening.
 *
 * That is what makes converting a path one at a time tractable: the audit is
 * not "is this safe against the whole kernel", it is "is this safe against the
 * short list of things that also run shared".
 *
 * Two kinds of lock come out of it:
 *
 *   oplock_globals  one lock over the state every shared path may touch --
 *                   the process queue, when a read wakes a blocked writer, and
 *                   the page allocator, when a copy faults a page in. Held for
 *                   the moments those happen, never across a copy.
 *
 *   per-object      a spinlock inside the pipe, the socket, the open file.
 *                   This is where the parallelism comes from: two processes
 *                   moving bytes through different pipes take different locks
 *                   and never meet.
 *
 * Order, when more than one is held: per-object first, then oplock_globals.
 * Nothing takes them the other way round, and nothing holds either across a
 * call that could sleep -- a shared path that finds it would have to block
 * gives up its shared ticket and starts again as an exclusive one.
 */

/*
 * Held while a shared path touches state every shared path may reach: the
 * process queue, the page allocator, an address space's page tables. Does
 * nothing when this processor holds the kernel lock exclusively, because an
 * exclusive holder is alone and has nobody to exclude. Recursive, so a call
 * that already holds it may call another that takes it.
 */
void oplock_enter(void);
void oplock_leave(void);

#endif
