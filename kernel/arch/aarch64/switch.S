    .section .text
    .globl  arch_switch_stack
    .globl  arch_user_resume_entry
    .globl  arch_kthread_entry
    .globl  arch_idle_entry

arch_switch_stack:
    sub     sp, sp, #96
    stp     x19, x20, [sp, #0]
    stp     x21, x22, [sp, #16]
    stp     x23, x24, [sp, #32]
    stp     x25, x26, [sp, #48]
    stp     x27, x28, [sp, #64]
    stp     x29, x30, [sp, #80]
    cbz     x0, 1f
    mov     x9, sp
    str     x9, [x0]
1:  mov     sp, x1
    ldp     x19, x20, [sp, #0]
    ldp     x21, x22, [sp, #16]
    ldp     x23, x24, [sp, #32]
    ldp     x25, x26, [sp, #48]
    ldp     x27, x28, [sp, #64]
    ldp     x29, x30, [sp, #80]
    add     sp, sp, #96
    ret

arch_user_resume_entry:
    bl      process_user_resume
    b       el0_return_to_user

arch_kthread_entry:
    mov     x0, x19
    mov     x1, x20
    bl      process_kthread_start
    brk     #0

arch_idle_entry:
    bl      process_idle_entry
    b       idle_loop
