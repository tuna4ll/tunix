.intel_syntax noprefix
.section .text
.global kernel_entry
.extern tunix_boot_start
.extern __bss_start
.extern kernel_end

kernel_entry:
    cli

    mov rax, cr0
    and rax, -5
    or rax, 2
    mov cr0, rax

    mov rax, cr4
    or rax, (1 << 9) | (1 << 10)
    mov cr4, rax

    lea rdi, [rip + __bss_start]
    lea rcx, [rip + kernel_end]
    sub rcx, rdi
    xor eax, eax
    rep stosb

    lea rsp, [rip + stack_top]
    xor rbp, rbp
    call tunix_boot_start
1:
    hlt
    jmp 1b

.section .bss
.align 16
stack_bottom:
    .skip 16384
stack_top:
