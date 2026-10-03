.intel_syntax noprefix
.section .text
.global arch_switch_stack
.global arch_user_resume_entry
.global arch_kthread_entry
.global arch_idle_entry
.extern process_user_resume
.extern process_kthread_start
.extern process_idle_entry
.extern syscall_return_to_user
.extern idle_loop

arch_switch_stack:
    push rbp
    push rbx
    push r12
    push r13
    push r14
    push r15
    test rdi, rdi
    jz 1f
    mov qword ptr [rdi], rsp
1:
    mov rsp, rsi
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret

arch_user_resume_entry:
    call process_user_resume
    jmp syscall_return_to_user

arch_kthread_entry:
    mov rdi, r12
    mov rsi, r13
    call process_kthread_start
    ud2

arch_idle_entry:
    call process_idle_entry
    jmp idle_loop
