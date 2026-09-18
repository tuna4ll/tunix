#include <stddef.h>
#include <stdint.h>

#include "../include/irq.h"

struct irq_slot {
    irq_handler_fn handler;
    void *context;
    const char *name;
    const char *kind;
    uint64_t count;
};

static struct irq_slot slots[IRQ_VECTOR_COUNT];
static uint64_t delivered;

unsigned irq_request(const char *name, const char *kind, irq_handler_fn handler,
                     void *context) {
    if (!handler) return 0;
    for (unsigned index = 0; index < IRQ_VECTOR_COUNT; index++) {
        if (slots[index].handler) continue;
        slots[index].context = context;
        slots[index].name = name ? name : "device";
        slots[index].kind = kind ? kind : "unknown";
        slots[index].count = 0;
        slots[index].handler = handler;
        return IRQ_VECTOR_FIRST + index;
    }
    return 0;
}

void irq_release(unsigned vector) {
    if (vector < IRQ_VECTOR_FIRST) return;
    unsigned index = vector - IRQ_VECTOR_FIRST;
    if (index >= IRQ_VECTOR_COUNT) return;
    slots[index].handler = NULL;
    slots[index].context = NULL;
    slots[index].name = NULL;
    slots[index].kind = NULL;
}

int irq_dispatch(unsigned vector) {
    if (vector < IRQ_VECTOR_FIRST) return 0;
    unsigned index = vector - IRQ_VECTOR_FIRST;
    if (index >= IRQ_VECTOR_COUNT) return 0;

    struct irq_slot *slot = &slots[index];
    slot->count++;
    delivered++;
    if (!slot->handler) return 0;
    slot->handler(slot->context);
    return 1;
}

int irq_describe(unsigned slot, unsigned *vector, uint64_t *count,
                 const char **name, const char **kind) {
    if (slot >= IRQ_VECTOR_COUNT) return -1;
    if (vector) *vector = IRQ_VECTOR_FIRST + slot;
    if (count) *count = slots[slot].count;
    if (name) *name = slots[slot].handler ? slots[slot].name : NULL;
    if (kind) *kind = slots[slot].handler ? slots[slot].kind : NULL;
    return 0;
}

uint64_t irq_total(void) { return delivered; }
