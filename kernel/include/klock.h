#ifndef TUNIX_KLOCK_H
#define TUNIX_KLOCK_H

/*
 * The lock a processor holds while it is inside the kernel.
 *
 * It used to be one lock, held exclusively for the whole of every syscall and
 * every interrupt, because nothing in this kernel was written to be entered
 * twice at once: the process queue is a bare linked list, the VFS tree has no
 * locks of its own, the page tables are edited in place. That is still true of
 * most of it, and most of it still takes this exclusively.
 *
 * What it cost was measured rather than guessed. Four processors running
 * `dd if=/dev/zero of=/dev/null` delivered 426 MB/s between them against
 * 479 MB/s from one -- *slower* than a single processor -- and sampling every
 * processor's instruction pointer put 96% of the samples inside the wait loop
 * below. User code parallelised almost perfectly over the same four (3.3x),
 * so the ceiling was never the scheduler; it was this.
 *
 * So it is now a lock with two modes:
 *
 *   exclusive  one holder, and no shared holders. What every path that has
 *              not been audited takes, which is why converting it changed
 *              nothing: the default is the old behaviour exactly.
 *   shared     any number of holders at once. A path may only take it this
 *              way once someone has established that everything it touches is
 *              either private to the calling process or protected by a lock
 *              of its own -- see the ordering note in oplock.h.
 *
 * Tickets rather than test-and-set, so that a processor entering the kernel in
 * a tight syscall loop cannot starve one that has been waiting. A shared
 * holder passes its ticket straight on, which is what lets the shared holders
 * behind it in; an exclusive holder keeps it until it leaves, and first waits
 * for the shared holders already inside to finish.
 *
 * Kernel mode runs with interrupts off, so a processor holding this can never
 * be interrupted into wanting it again, and the wait is always finite.
 *
 * That reasoning covers interrupts and nothing else. An *exception* is not
 * maskable: a kernel-mode page fault arrives however the flags are set, and
 * the entry path that answers it would take this lock a second time on a
 * processor that already holds it -- and a ticket lock has no way to satisfy
 * that, because the holder is the one waiting. The machine would stop dead
 * with every other processor queued behind it. That is what klock_held_here()
 * is for, and why the entry path asks before taking anything.
 */

/* Exclusive: the old behaviour, and still the default everywhere. */
void kernel_lock(void);
void kernel_unlock(void);

/* Shared: many at once. Only for paths audited against the rule above. */
void kernel_lock_shared(void);
void kernel_unlock_shared(void);

/* Release whichever mode this processor holds. The entry stubs call this,
   because the mode is chosen per syscall after the stub has been entered. */
void kernel_unlock_current(void);

/* Whether this processor is inside the lock at all, in either mode. Only the
   exception entry path needs it, and only to tell "a fault from user code"
   (take the lock as usual) from "a fault the kernel itself took while holding
   it" (do not, and do not release it either). */
int kernel_lock_held_here(void);

/* The pair the interrupt path uses. Taking it only when it is free is safe
   only if it is released only when it was taken: see klock.c. */
/* Leave a note about what this processor is doing, for the lock watchdog to
   print when somebody stops giving the lock back. KLOCK_NOTE_* below. */
void klock_note(uint32_t what);

#define KLOCK_NOTE_SYSCALL   0x10000U  /* | the syscall number */
#define KLOCK_NOTE_INTERRUPT 0x20000U  /* | the vector */
#define KLOCK_NOTE_FIRST_RUN 0x30000U
#define KLOCK_NOTE_IDLE      0x40000U

void kernel_lock_from_isr(void);
void kernel_unlock_from_isr(void);
/* Which mode, for the paths that have to give a shared holder back its ticket
   before doing something only an exclusive holder may do. */
int kernel_lock_shared_here(void);

#endif
