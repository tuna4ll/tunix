    .section .text

    // aarch64_context_switch(uint64_t *save_sp, uint64_t new_sp)
    // Saves the callee-saved set on the outgoing kernel stack and adopts
    // the incoming one; the matching ret resumes wherever that task slept.
    .globl aarch64_context_switch
aarch64_context_switch:
    sub     sp, sp, #96
    stp     x19, x20, [sp, #16 * 0]
    stp     x21, x22, [sp, #16 * 1]
    stp     x23, x24, [sp, #16 * 2]
    stp     x25, x26, [sp, #16 * 3]
    stp     x27, x28, [sp, #16 * 4]
    stp     x29, x30, [sp, #16 * 5]
    mov     x9, sp
    str     x9, [x0]

    mov     sp, x1
    ldp     x19, x20, [sp, #16 * 0]
    ldp     x21, x22, [sp, #16 * 1]
    ldp     x23, x24, [sp, #16 * 2]
    ldp     x25, x26, [sp, #16 * 3]
    ldp     x27, x28, [sp, #16 * 4]
    ldp     x29, x30, [sp, #16 * 5]
    add     sp, sp, #96
    ret

    // A fresh task lands here with x19 = entry, x20 = argument.
    .globl task_trampoline
task_trampoline:
    msr     daifclr, #2             // a new task starts preemptible
    mov     x0, x20
    blr     x19
    bl      sched_exit
1:  b       1b
