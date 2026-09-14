#include <stdint.h>

#include "include/cpu.h"
#include "include/klock.h"
#include "include/oplock.h"
#include "include/percpu.h"

static volatile uint32_t holder;
static uint32_t depth;

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
        cpu_relax();
    }
    depth = 1;
}

void oplock_leave(void) {
    if (!kernel_lock_shared_here()) return;
    if (--depth == 0) __atomic_store_n(&holder, 0U, __ATOMIC_RELEASE);
}
