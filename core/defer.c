#include <stddef.h>
#include <stdint.h>

#include <tunix/defer.h>
#include <tunix/heap.h>
#include <tunix/lock.h>
#include <tunix/percpu.h>

struct quiescence {
    volatile uint32_t depth;
    volatile uint64_t entered;
} __attribute__((aligned(64)));

static struct quiescence cpus[SMP_MAX_CPUS];
static volatile uint64_t generation = 1;
static struct lock pending_lock = LOCK_INITIALIZER("deferred frees", LOCK_RANK_LEAF);
static struct defer_item *pending;
static volatile uint64_t pending_count;
static struct defer_park *parked;

void defer_kernel_enter(void) {
    struct quiescence *self = &cpus[cpu_current()->index];
    if (self->depth++) return;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    __atomic_store_n(&self->entered, __atomic_load_n(&generation, __ATOMIC_SEQ_CST),
                     __ATOMIC_SEQ_CST);
}

unsigned defer_cpus_in_kernel(void) {
    unsigned inside = 0;
    for (unsigned index = 0; index < SMP_MAX_CPUS; index++)
        if (__atomic_load_n(&cpus[index].depth, __ATOMIC_RELAXED)) inside++;
    return inside;
}

int defer_in_kernel(void) { return cpus[cpu_current()->index].depth != 0; }

void defer_park(struct defer_park *park) {
    struct quiescence *self = &cpus[cpu_current()->index];
    park->depth = self->depth;
    park->entered = self->entered;
    if (park->depth) {
        lock_acquire(&pending_lock);
        park->prev = NULL;
        park->next = parked;
        if (parked) parked->prev = park;
        parked = park;
        park->listed = 1;
        lock_release(&pending_lock);
    }
    __atomic_store_n(&self->depth, 0, __ATOMIC_RELEASE);
}

void defer_unpark(struct defer_park *park) {
    struct quiescence *self = &cpus[cpu_current()->index];
    __atomic_store_n(&self->entered, park->entered, __ATOMIC_SEQ_CST);
    __atomic_store_n(&self->depth, park->depth, __ATOMIC_SEQ_CST);
    if (!park->listed) return;
    lock_acquire(&pending_lock);
    if (park->prev) park->prev->next = park->next;
    else parked = park->next;
    if (park->next) park->next->prev = park->prev;
    park->prev = park->next = NULL;
    park->listed = 0;
    lock_release(&pending_lock);
}

void defer_cpu_reset(uint32_t depth) {
    struct quiescence *self = &cpus[cpu_current()->index];
    if (!depth) {
        __atomic_store_n(&self->depth, 0, __ATOMIC_RELEASE);
        return;
    }
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    __atomic_store_n(&self->entered, __atomic_load_n(&generation, __ATOMIC_SEQ_CST),
                     __ATOMIC_SEQ_CST);
    __atomic_store_n(&self->depth, depth, __ATOMIC_SEQ_CST);
}

void defer_kernel_leave(void) {
    struct quiescence *self = &cpus[cpu_current()->index];
    if (!self->depth) return;
    __atomic_store_n(&self->depth, self->depth - 1U, __ATOMIC_RELEASE);
}

void defer_release(struct defer_item *item, void (*release)(struct defer_item *item)) {
    if (!item) return;
    item->release = release;
    item->generation = __atomic_add_fetch(&generation, 1, __ATOMIC_SEQ_CST);
    lock_acquire(&pending_lock);
    item->next = pending;
    pending = item;
    pending_count++;
    lock_release(&pending_lock);
}

static void release_block(struct defer_item *item) { kfree(item); }

void *defer_alloc(uint64_t size) {
    struct defer_item *item = (struct defer_item *)kmalloc(sizeof(*item) + size);
    return item ? (void *)(item + 1) : NULL;
}

void defer_free(void *pointer) {
    if (!pointer) return;
    defer_release((struct defer_item *)pointer - 1, release_block);
}

static uint64_t oldest_in_kernel(void) {
    uint64_t oldest = UINT64_MAX;
    for (unsigned index = 0; index < SMP_MAX_CPUS; index++) {
        struct cpu *cpu = percpu_slot(index);
        if (!cpu || !cpu->online) continue;
        if (!__atomic_load_n(&cpus[index].depth, __ATOMIC_SEQ_CST)) continue;
        uint64_t entered = __atomic_load_n(&cpus[index].entered, __ATOMIC_SEQ_CST);
        if (entered < oldest) oldest = entered;
    }
    return oldest;
}

void defer_poll(void) {
    if (!__atomic_load_n(&pending_count, __ATOMIC_RELAXED)) return;
    uint64_t oldest = oldest_in_kernel();
    struct defer_item *ready = NULL;
    lock_acquire(&pending_lock);
    for (struct defer_park *park = parked; park; park = park->next)
        if (park->entered < oldest) oldest = park->entered;
    for (struct defer_item **link = &pending; *link;) {
        struct defer_item *item = *link;
        if (item->generation <= oldest) {
            *link = item->next;
            item->next = ready;
            ready = item;
            pending_count--;
        } else {
            link = &item->next;
        }
    }
    lock_release(&pending_lock);
    while (ready) {
        struct defer_item *next = ready->next;
        ready->release(ready);
        ready = next;
    }
}
