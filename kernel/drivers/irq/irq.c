#include <stddef.h>
#include <stdint.h>

#include <tunix/irq.h>
#include <tunix/lock.h>

struct irq_slot {
    irq_handler_fn handler;
    void *context;
    const char *name;
    const char *kind;
    uint64_t count;
};

static struct irq_slot slots[IRQ_VECTOR_COUNT];
static uint64_t delivered;
static struct lock slots_lock = LOCK_INITIALIZER("irq slots", LOCK_RANK_LEAF);

unsigned irq_request(const char *name, const char *kind, irq_handler_fn handler,
                     void *context) {
    if (!handler) return 0;
    lock_acquire(&slots_lock);
    for (unsigned index = 0; index < IRQ_VECTOR_COUNT; index++) {
        if (slots[index].handler) continue;
        slots[index].context = context;
        slots[index].name = name ? name : "device";
        slots[index].kind = kind ? kind : "unknown";
        slots[index].count = 0;
        __atomic_store_n(&slots[index].handler, handler, __ATOMIC_RELEASE);
        lock_release(&slots_lock);
        return IRQ_VECTOR_FIRST + index;
    }
    lock_release(&slots_lock);
    return 0;
}

void irq_release(unsigned vector) {
    if (vector < IRQ_VECTOR_FIRST) return;
    unsigned index = vector - IRQ_VECTOR_FIRST;
    if (index >= IRQ_VECTOR_COUNT) return;
    lock_acquire(&slots_lock);
    __atomic_store_n(&slots[index].handler, NULL, __ATOMIC_RELEASE);
    slots[index].context = NULL;
    slots[index].name = NULL;
    slots[index].kind = NULL;
    lock_release(&slots_lock);
}

int irq_dispatch(unsigned vector) {
    if (vector < IRQ_VECTOR_FIRST) return 0;
    unsigned index = vector - IRQ_VECTOR_FIRST;
    if (index >= IRQ_VECTOR_COUNT) return 0;

    struct irq_slot *slot = &slots[index];
    __atomic_fetch_add(&slot->count, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&delivered, 1, __ATOMIC_RELAXED);
    irq_handler_fn handler = __atomic_load_n(&slot->handler, __ATOMIC_ACQUIRE);
    if (!handler) return 0;
    handler(slot->context);
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
