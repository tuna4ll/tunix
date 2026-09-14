#include <stddef.h>
#include <stdint.h>

#include "arch.h"

#define MAX_TASKS   8
#define STACK_BYTES 16384

#define TASK_FREE  0
#define TASK_READY 1
#define TASK_DONE  2

extern char task_trampoline[];

struct task {
    uint64_t sp;                    // saved kernel stack pointer
    uint64_t resume[2];             // where enter_user must come back to
    uint64_t space;                 // TTBR0 root, 0 for a kernel-only task
    void *stack;
    int state;
    int id;
    const char *name;
};

static struct task tasks[MAX_TASKS];
static struct task *current;

uint64_t *current_resume;

static uint64_t irq_save(void) {
    uint64_t daif = sysreg_read("daif");
    __asm__ volatile("msr daifset, #2" ::: "memory");
    return daif;
}

static void irq_restore(uint64_t daif) {
    sysreg_write("daif", daif);
}

static void switch_to(struct task *next) {
    struct task *previous = current;
    current = next;
    current_resume = next->resume;
    if (next->space) vmm_switch_space(next->space);
    aarch64_context_switch(&previous->sp, next->sp);
}

static void schedule(void) {
    if (!current) return;
    unsigned start = (unsigned)(current - tasks);

    for (unsigned step = 1; step <= MAX_TASKS; step++) {
        struct task *candidate = &tasks[(start + step) % MAX_TASKS];

        if (candidate->state == TASK_DONE && candidate != current) {
            kfree(candidate->stack);            // safe: not the stack in use
            candidate->stack = NULL;
            candidate->state = TASK_FREE;
            continue;
        }
        if (candidate->state != TASK_READY || candidate == current) continue;

        switch_to(candidate);
        return;
    }
}

void sched_init(void) {
    tasks[0].state = TASK_READY;
    tasks[0].id = 0;
    tasks[0].name = "idle";
    current = &tasks[0];
    current_resume = tasks[0].resume;
}

int sched_create(const char *name, void (*entry)(void *), void *argument) {
    for (int i = 1; i < MAX_TASKS; i++) {
        struct task *task = &tasks[i];
        if (task->state != TASK_FREE) continue;

        task->stack = kmalloc(STACK_BYTES);
        if (!task->stack) return -1;

        uint64_t *frame = (uint64_t *)((uint64_t)task->stack + STACK_BYTES);
        frame -= 12;                            // x19..x30, as switch.S saves them
        for (int slot = 0; slot < 12; slot++) frame[slot] = 0;
        frame[0] = (uint64_t)entry;             // x19
        frame[1] = (uint64_t)argument;          // x20
        frame[11] = (uint64_t)task_trampoline;  // x30

        task->sp = (uint64_t)frame;
        task->space = 0;
        task->id = i;
        task->name = name;
        task->state = TASK_READY;
        return i;
    }
    return -1;
}

void sched_yield(void) {
    uint64_t daif = irq_save();
    schedule();
    irq_restore(daif);
}

void sched_exit(void) {
    uint64_t daif = irq_save();
    current->state = TASK_DONE;
    irq_restore(daif);
    for (;;) sched_yield();                     // a done task is never picked again
}

void sched_tick(void) {
    schedule();                                 // already inside the IRQ handler
}

void sched_set_space(uint64_t root_pa) {
    current->space = root_pa;
    vmm_switch_space(root_pa);
}

int sched_current_id(void) {
    return current ? current->id : -1;
}
