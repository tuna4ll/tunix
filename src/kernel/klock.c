#include <stdint.h>

#include "include/klock.h"
#include "include/percpu.h"
#include "include/smp.h"

/* See include/klock.h for what the two modes are and what was measured. */

static volatile uint32_t next_ticket;
static volatile uint32_t now_serving;
/* How many processors are inside in shared mode. An exclusive holder waits for
   this to reach zero before it starts, which is what makes the two exclusive
   of one another. */
static volatile int32_t shared_holders;

#define KLOCK_MODE_NONE 0
#define KLOCK_MODE_EXCLUSIVE 1
#define KLOCK_MODE_SHARED 2

/* What each processor is holding, indexed by processor. Only the processor
   itself writes its own entry, so no lock protects this. */
static volatile uint8_t held_mode[SMP_MAX_CPUS];

/*
 * Wait for this ticket to come up. The flush service runs inside the wait
 * because interrupts are off: a processor asking this one to drop cached
 * translations cannot arrive as an interrupt, and the processor doing the
 * asking may be the very one holding the lock this is waiting for.
 */
static void wait_for_turn(uint32_t ticket) {
    while (__atomic_load_n(&now_serving, __ATOMIC_ACQUIRE) != ticket) {
        smp_service_flush();
        __asm__ volatile("pause");
    }
}

void kernel_lock(void) {
    uint32_t ticket = __atomic_fetch_add(&next_ticket, 1, __ATOMIC_RELAXED);
    wait_for_turn(ticket);
    /* The ticket is ours, but shared holders admitted before it may still be
       inside. They cannot grow in number -- their tickets are behind ours. */
    while (__atomic_load_n(&shared_holders, __ATOMIC_ACQUIRE) != 0) {
        smp_service_flush();
        __asm__ volatile("pause");
    }
    held_mode[cpu_current()->index] = KLOCK_MODE_EXCLUSIVE;
}

void kernel_unlock(void) {
    held_mode[cpu_current()->index] = KLOCK_MODE_NONE;
    __atomic_store_n(&now_serving, now_serving + 1, __ATOMIC_RELEASE);
}

/*
 * Shared entry, without taking a ticket when there is nobody to queue behind.
 *
 * Taking one costs three read-modify-writes on two shared words for every
 * syscall, and the processors then hand the ticket round one at a time -- which
 * showed up as 17% of all samples once the paths that matter had been moved
 * here. So the common case is a single increment and two loads: announce
 * ourselves, then check that no exclusive holder was already inside or queued.
 *
 * The check has to come after the increment, not before. An exclusive holder
 * waits for this counter to reach zero, so a processor that announced itself
 * first is one the exclusive holder will wait for; one that looked first and
 * announced afterwards could slip in behind its back.
 *
 * If an exclusive holder is queued we stand down and take a ticket after all,
 * which is what keeps it from being starved by a stream of shared arrivals.
 */
void kernel_lock_shared(void) {
    __atomic_fetch_add(&shared_holders, 1, __ATOMIC_ACQUIRE);
    if (__atomic_load_n(&next_ticket, __ATOMIC_ACQUIRE) !=
        __atomic_load_n(&now_serving, __ATOMIC_ACQUIRE)) {
        __atomic_fetch_sub(&shared_holders, 1, __ATOMIC_RELEASE);
        uint32_t ticket = __atomic_fetch_add(&next_ticket, 1, __ATOMIC_RELAXED);
        wait_for_turn(ticket);
        __atomic_fetch_add(&shared_holders, 1, __ATOMIC_ACQUIRE);
        __atomic_store_n(&now_serving, now_serving + 1, __ATOMIC_RELEASE);
    }
    held_mode[cpu_current()->index] = KLOCK_MODE_SHARED;
}

void kernel_unlock_shared(void) {
    held_mode[cpu_current()->index] = KLOCK_MODE_NONE;
    __atomic_fetch_sub(&shared_holders, 1, __ATOMIC_RELEASE);
}

int kernel_lock_held_here(void) {
    return held_mode[cpu_current()->index] != KLOCK_MODE_NONE;
}

int kernel_lock_shared_here(void) {
    return held_mode[cpu_current()->index] == KLOCK_MODE_SHARED;
}

/*
 * Release whichever mode this processor took.
 *
 * The entry stubs call this by name on the way out and have no way to know
 * which of the two the dispatcher chose -- that decision is made per syscall,
 * in C, after the stub has already been entered. Keeping the choice in one
 * place here is what lets the assembly stay a single unconditional call.
 */
void kernel_unlock_current(void) {
    if (held_mode[cpu_current()->index] == KLOCK_MODE_SHARED) kernel_unlock_shared();
    else kernel_unlock();
}
