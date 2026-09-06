#ifndef TUNIX_KLOCK_H
#define TUNIX_KLOCK_H

/*
 * The lock a processor holds while it is inside the kernel, in two modes:
 * exclusive, which every path that has not been audited takes, and shared, for
 * one whose every touch is private to the caller or has a lock of its own (see
 * oplock.h). Tickets, so a tight syscall loop cannot starve a waiter. Kernel
 * mode runs with interrupts off, but an exception is not maskable, which is
 * what kernel_lock_held_here() is for.
 */

/* Exclusive: the old behaviour, and still the default everywhere. */
/* The worst exclusive holds, filed under the breadcrumb of whoever took the
   lock; see /proc/klock. Recording is off until klock_statistics_enable(). */
#define KLOCK_HOLD_SLOTS 24U

struct klock_hold {
    uint32_t note;
    uint32_t count;
    uint64_t total_ns;
    uint64_t max_ns;
};

void klock_statistics_start(void);
void klock_statistics_stop(void);
int klock_statistics(unsigned index, struct klock_hold *out);

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
/* | the request, for the one syscall whose cost is all in which request it is. */
#define KLOCK_NOTE_IOCTL     0x50000U

void kernel_lock_from_isr(void);
void kernel_unlock_from_isr(void);
/* Which mode, for the paths that have to give a shared holder back its ticket
   before doing something only an exclusive holder may do. */
int kernel_lock_shared_here(void);

#endif
