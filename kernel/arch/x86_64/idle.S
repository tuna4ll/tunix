.intel_syntax noprefix
.section .text
.global cpu_enter_idle
.global cpu_idle_park
.extern kernel_unlock_current

/*
 * Park a processor on its idle stack.
 *
 * The stack it was on is thrown away rather than unwound: whatever brought it
 * here has nothing left worth returning to, and the process state it was
 * working on is already written down. rsp is reset to the top of the idle
 * stack every time, so the abandoned frames cost nothing.
 *
 * `sti; hlt` is one step, not two: STI leaves interrupts blocked for exactly
 * the following instruction, so the tick that would have arrived in between
 * arrives after the halt instead of before it -- otherwise a processor could
 * miss its wakeup and stay asleep with work waiting.
 */
/* kernel_unlock_current rather than kernel_unlock, because the mode is chosen
   per syscall in C and this is reached from paths that could hold either: an
   unlock that names the wrong one moves the ticket queue past a ticket nobody
   was serving, and every later attempt then waits for a turn that has already
   gone by. */
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

idle_loop:
    sti
    hlt
    cli
    jmp idle_loop
