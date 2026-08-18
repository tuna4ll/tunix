#ifndef TUNIX_KLOCK_H
#define TUNIX_KLOCK_H

/*
 * One lock, held for the whole time a processor is inside the kernel.
 *
 * Nothing in this kernel was written to be entered twice at once: the process
 * queue is a bare linked list, the VFS tree has no locks, the page tables are
 * edited in place. Making each of those safe separately is a long job with a
 * long tail of races that only appear under load, and none of it is needed to
 * get work onto more than one processor -- user code is where the time goes,
 * and user code does not hold this.
 *
 * So the rule is the simple one: take it on the way in, drop it on the way
 * out, and let the processors run in parallel everywhere else. Kernel mode
 * runs with interrupts off, so a processor holding this can never be
 * interrupted into wanting it again, and the wait is always finite.
 *
 * That reasoning covers interrupts and nothing else. An *exception* is not
 * maskable: a kernel-mode page fault arrives however the flags are set, and
 * the entry path that answers it would take this lock a second time on a
 * processor that already holds it. A ticket lock has no way to satisfy that
 * -- the holder is the one waiting -- so the machine stops dead, with every
 * other processor queued behind it and no signal able to reach any of them.
 * A kernel bug that should have printed a fault address instead looks like a
 * hang, which is why the entry path asks kernel_lock_held_here() first.
 *
 * It is a ticket lock rather than a test-and-set so that a processor entering
 * the kernel in a tight syscall loop cannot starve one that has been waiting.
 */
void kernel_lock(void);
void kernel_unlock(void);
/* Whether this processor is the one inside the lock. Only the exception entry
   path needs it, and only to tell "a fault from user code" (take the lock as
   usual) from "a fault the kernel itself took while holding it" (do not). */
int kernel_lock_held_here(void);

#endif
