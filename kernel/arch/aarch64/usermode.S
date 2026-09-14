    .section .text

    // Drop to EL0 at x0 with stack x1, remembering how to get back.
    .globl aarch64_enter_user
aarch64_enter_user:
    adrp    x9, kernel_resume
    add     x9, x9, :lo12:kernel_resume
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
    adrp    x9, kernel_resume
    add     x9, x9, :lo12:kernel_resume
    ldp     x10, x30, [x9]
    mov     sp, x10
    ret

    .section .bss
    .align 3
kernel_resume:
    .space 16

    // A page-aligned blob run at EL0; adr keeps it position independent.
    .section .rodata.usertest, "a"
    .align 12
    .globl usertest_start
usertest_start:
    mov     x8, #64                 // __NR_write
    mov     x0, #1                  // stdout
    adr     x1, usertest_msg
    mov     x2, #(usertest_msg_end - usertest_msg)
    svc     #0

    mov     x8, #93                 // __NR_exit
    mov     x0, #7
    svc     #0
1:  b       1b

usertest_msg:
    .ascii  "hello from EL0, through svc\n"
usertest_msg_end:

    .align 12
    .globl usertest_end
usertest_end:
