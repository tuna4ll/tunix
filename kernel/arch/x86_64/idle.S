.intel_syntax noprefix
.section .text
.global cpu_enter_idle
.global cpu_idle_park
.extern kernel_unlock_current

/* Park a processor on its idle stack, which is abandoned rather than unwound
   because everything worth keeping is already written into the process. */
/* rsp is reset to the top of the idle stack every time, so the frames left
   behind by whatever came here cost nothing. */
/* kernel_unlock_current rather than kernel_unlock, because the mode is chosen
   per syscall in C and an unlock that names the wrong one moves the ticket
   queue past a ticket nobody was serving. */
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

/* `sti; hlt` is one step and not two, because STI leaves interrupts blocked for
   exactly the following instruction and a tick that arrived in between would
   otherwise leave this processor asleep with work waiting. */
idle_loop:
    sti
    hlt
    cli
    jmp idle_loop
