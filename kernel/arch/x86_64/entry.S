.intel_syntax noprefix
.section .text
.global kernel_entry
.extern limine_start
.extern __bss_start
.extern kernel_end

kernel_entry:
    cli
    /* Before the first string instruction, which is the one below that clears
       the BSS. The handover is not a function call, so nothing promises the
       direction flag is clear -- and a `rep stos` that walks backwards from
       __bss_start erases the kernel's own text and data before a single line
       has been printed. Every interrupt entry clears it for the same reason;
       this is the one place that runs before any of them. */
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
    call limine_start
1:
    hlt
    jmp 1b

.section .bss
.align 16
stack_bottom:
    .skip 16384
stack_top:
