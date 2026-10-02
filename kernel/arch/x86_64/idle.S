.intel_syntax noprefix
.section .text
.global cpu_enter_idle
.global cpu_idle_park
.extern kernel_exit

cpu_enter_idle:
    cli
    mov rsp, rdi
    xor rbp, rbp
    call kernel_exit
    jmp idle_loop

cpu_idle_park:
    cli
    mov rsp, rdi
    xor rbp, rbp

idle_loop:
    sti
    hlt
    cli
    jmp idle_loop
