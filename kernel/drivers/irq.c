/*
 * The table behind irq.h: sixteen vectors, and who answers each of them.
 *
 * There is no locking here on purpose. Every path into this file already holds
 * the kernel lock -- irq_request() is called from driver init, irq_dispatch()
 * from isr_handler() after kernel_lock_from_isr() -- so the counters are
 * ordinary increments rather than atomics, and the table is written before the
 * vector it describes is ever routed.
 */
#include <stddef.h>
#include <stdint.h>

#include "../include/irq.h"

struct irq_slot {
    irq_handler_fn handler;
    void *context;
    const char *name;
    uint64_t count;
};

static struct irq_slot slots[IRQ_VECTOR_COUNT];
static uint64_t delivered;

unsigned irq_request(const char *name, irq_handler_fn handler, void *context) {
    if (!handler) return 0;
    for (unsigned index = 0; index < IRQ_VECTOR_COUNT; index++) {
        if (slots[index].handler) continue;
        slots[index].context = context;
        slots[index].name = name ? name : "device";
        slots[index].count = 0;
        /* Last, and deliberately: the vector is already in the IDT, so a
           half-filled slot is one an interrupt could arrive into. */
        slots[index].handler = handler;
        return IRQ_VECTOR_FIRST + index;
    }
    return 0;
}

int irq_dispatch(unsigned vector) {
    if (vector < IRQ_VECTOR_FIRST) return 0;
    unsigned index = vector - IRQ_VECTOR_FIRST;
    if (index >= IRQ_VECTOR_COUNT) return 0;

    struct irq_slot *slot = &slots[index];
    /* Counted even when nobody claimed the vector. An interrupt arriving on a
       vector with no handler is worth being able to see, and it is the shape a
       device left enabled by the firmware takes. */
    slot->count++;
    delivered++;
    if (!slot->handler) return 0;
    slot->handler(slot->context);
    return 1;
}

int irq_describe(unsigned slot, unsigned *vector, uint64_t *count,
                 const char **name) {
    if (slot >= IRQ_VECTOR_COUNT) return -1;
    if (vector) *vector = IRQ_VECTOR_FIRST + slot;
    if (count) *count = slots[slot].count;
    if (name) *name = slots[slot].handler ? slots[slot].name : NULL;
    return 0;
}

uint64_t irq_total(void) { return delivered; }
