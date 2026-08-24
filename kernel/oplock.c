#include <stdint.h>

#include "include/klock.h"
#include "include/oplock.h"
#include "include/percpu.h"

/* See include/oplock.h for the rule these implement. */

static volatile uint32_t holder;   /* processor index + 1, zero for nobody */
/* Only the holder reads or writes this, so it needs no atomics of its own. */
static uint32_t depth;

/*
 * Recursive, and deliberately so.
 *
 * The state a shared path touches is reached through calls that also reach it
 * from each other: committing a page allocates one, and both of those want
 * this. Making each caller track whether it already holds it would put the
 * ordering rule in every call site, where it would eventually be got wrong;
 * counting the depth here puts it in one place that cannot be.
 *
 * An exclusive holder is alone in the kernel and takes nothing.
 */
void oplock_enter(void) {
    if (!kernel_lock_shared_here()) return;
    uint32_t me = cpu_current()->index + 1U;
    if (__atomic_load_n(&holder, __ATOMIC_RELAXED) == me) {
        depth++;
        return;
    }
    for (;;) {
        uint32_t nobody = 0;
        if (__atomic_compare_exchange_n(&holder, &nobody, me, 0,
                                        __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
            break;
        }
        /* Whoever has it is another processor in shared mode doing something
           short -- a wake, a page -- so spinning is the right wait. */
        __asm__ volatile("pause");
    }
    depth = 1;
}

void oplock_leave(void) {
    if (!kernel_lock_shared_here()) return;
    if (--depth == 0) __atomic_store_n(&holder, 0U, __ATOMIC_RELEASE);
}
