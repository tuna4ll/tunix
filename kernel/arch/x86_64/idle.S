.intel_syntax noprefix
.section .text
.global cpu_enter_idle
.global cpu_idle_park
.extern kernel_unlock_current

/* Park a processor on its idle stack, abandoned rather than unwound,
   releasing whichever mode it held. */
cpu_enter_idle:
    cli
    mov rsp, rdi
    xor rbp, rbp
    call kernel_unlock_current
    jmp idle_loop

/* The same, for a processor that has just come up and holds nothing. */
cpu_idle_park:
    cli
    mov rsp, rdi
    xor rbp, rbp

/* `sti; hlt` is one step, because STI blocks interrupts for exactly the
   next instruction. */
idle_loop:
    sti
    hlt
    cli
    jmp idle_loop
