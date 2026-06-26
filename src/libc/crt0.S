.section .text
.global _start
.type _start,@function
.extern __tunix_start
_start:
    xor %rbp, %rbp
    mov (%rsp), %rdi
    lea 8(%rsp), %rsi
    lea 16(%rsp,%rdi,8), %rdx
    andq $-16, %rsp
    call __tunix_start
    ud2
.size _start, .-_start
