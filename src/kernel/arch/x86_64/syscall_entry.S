.intel_syntax noprefix
.section .bss
.align 16
syscall_kernel_rsp:
    .quad 0
syscall_user_rsp:
    .quad 0

.section .text
.global syscall_entry
.global syscall_set_kernel_stack
.global process_enter_user
.extern syscall_dispatch

syscall_entry:
    mov qword ptr [rip + syscall_user_rsp], rsp
    mov rsp, qword ptr [rip + syscall_kernel_rsp]
    test rsp, rsp
    jz syscall_fatal

    sub rsp, 128
    mov qword ptr [rsp + 0], r15
    mov qword ptr [rsp + 8], r14
    mov qword ptr [rsp + 16], r13
    mov qword ptr [rsp + 24], r12
    mov qword ptr [rsp + 32], rbp
    mov qword ptr [rsp + 40], rbx
    mov qword ptr [rsp + 48], r9
    mov qword ptr [rsp + 56], r8
    mov qword ptr [rsp + 64], r10
    mov qword ptr [rsp + 72], rdx
    mov qword ptr [rsp + 80], rsi
    mov qword ptr [rsp + 88], rdi
    mov qword ptr [rsp + 96], rax
    mov qword ptr [rsp + 104], rcx
    mov qword ptr [rsp + 112], r11
    mov rax, qword ptr [rip + syscall_user_rsp]
    mov qword ptr [rsp + 120], rax

    mov rdi, rsp
    call syscall_dispatch

    mov rax, qword ptr [rsp + 120]
    mov qword ptr [rip + syscall_user_rsp], rax
    mov r15, qword ptr [rsp + 0]
    mov r14, qword ptr [rsp + 8]
    mov r13, qword ptr [rsp + 16]
    mov r12, qword ptr [rsp + 24]
    mov rbp, qword ptr [rsp + 32]
    mov rbx, qword ptr [rsp + 40]
    mov r9, qword ptr [rsp + 48]
    mov r8, qword ptr [rsp + 56]
    mov r10, qword ptr [rsp + 64]
    mov rdx, qword ptr [rsp + 72]
    mov rsi, qword ptr [rsp + 80]
    mov rdi, qword ptr [rsp + 88]
    mov rax, qword ptr [rsp + 96]
    mov rcx, qword ptr [rsp + 104]
    mov r11, qword ptr [rsp + 112]
    mov rsp, qword ptr [rip + syscall_user_rsp]
    sysretq

syscall_fatal:
    cli
1:
    hlt
    jmp 1b

syscall_set_kernel_stack:
    mov qword ptr [rip + syscall_kernel_rsp], rdi
    ret

process_enter_user:
    cli
    mov rax, rdx
    mov cr3, rax
    mov ax, 0x1b
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    push 0x1b
    push rsi
    push 0x202
    push 0x23
    push rdi
    iretq
