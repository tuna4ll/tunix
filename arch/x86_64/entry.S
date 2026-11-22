.intel_syntax noprefix
.section .text
.global kernel_entry
.global stub_kernel_entry
.extern limine_start
.extern stub_start
.extern __bss_start
.extern kernel_end

// Limine enters here, with nothing in the registers.
kernel_entry:
    lea r12, [rip + limine_start]
    xor r13, r13
    jmp common_entry

// arch/x86_64/bzimage enters here, with its handoff in rdi.
stub_kernel_entry:
    lea r12, [rip + stub_start]
    mov r13, rdi

common_entry:
    cli
    cld

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
    mov rdi, r13
    call r12
1:
    hlt
    jmp 1b

.section .bss
.align 16
stack_bottom:
    .skip 16384
stack_top:
