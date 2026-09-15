    .section .text

    // Drop to EL0 at x0 with stack x1, remembering how to get back.
    // The resume slot is per task, so a preempted EL0 task returns correctly.
    .globl aarch64_enter_user
aarch64_enter_user:
    adrp    x9, current_resume
    add     x9, x9, :lo12:current_resume
    ldr     x9, [x9]
    mov     x10, sp
    stp     x10, x30, [x9]

    msr     sp_el0, x1
    msr     elr_el1, x0
    mov     x2, #0                  // EL0t, interrupts unmasked
    msr     spsr_el1, x2
    isb
    eret

    // Abandon the user context and return as if enter_user had returned.
    .globl aarch64_leave_user
aarch64_leave_user:
    adrp    x9, current_resume
    add     x9, x9, :lo12:current_resume
    ldr     x9, [x9]
    ldp     x10, x30, [x9]
    mov     sp, x10
    ret
