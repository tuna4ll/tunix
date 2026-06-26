.section .text
.global __tunix_sigreturn
.type __tunix_sigreturn,@function
__tunix_sigreturn:
    mov $15, %rax
    syscall
    ud2
.size __tunix_sigreturn, .-__tunix_sigreturn
