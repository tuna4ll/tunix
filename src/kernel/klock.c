#include <stdint.h>

#include "include/klock.h"
#include "include/percpu.h"
#include "include/smp.h"

static volatile uint32_t next_ticket;
static volatile uint32_t now_serving;
/* Which processor is inside the lock, as index+1 so that zero means nobody.
   Only the holder writes it, and only ever to its own value or back to zero. */
static volatile uint32_t owner;

void kernel_lock(void) {
    uint32_t ticket = __atomic_fetch_add(&next_ticket, 1, __ATOMIC_RELAXED);
    while (__atomic_load_n(&now_serving, __ATOMIC_ACQUIRE) != ticket) {
        /* Interrupts are off while waiting here, so the request to drop
           cached translations cannot arrive as one. Answering it in the wait
           loop is what keeps the processor that is holding the lock -- and
           waiting for this answer -- from waiting for ever. */
        smp_service_flush();
        __asm__ volatile("pause");
    }
    __atomic_store_n(&owner, cpu_current()->index + 1U, __ATOMIC_RELAXED);
}

void kernel_unlock(void) {
    __atomic_store_n(&owner, 0U, __ATOMIC_RELAXED);
    __atomic_store_n(&now_serving, now_serving + 1, __ATOMIC_RELEASE);
}

int kernel_lock_held_here(void) {
    return __atomic_load_n(&owner, __ATOMIC_RELAXED) == cpu_current()->index + 1U;
}
