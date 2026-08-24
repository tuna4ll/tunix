.intel_syntax noprefix
.section .text
.global syscall_entry
.global process_enter_user
.extern syscall_dispatch
.extern kernel_unlock_current

/* struct cpu, as percpu.h lays it out. */
.set CPU_KERNEL_RSP, 0
.set CPU_USER_RSP, 8

/*
 * SYSCALL overwrites RCX with the return RIP and R11 with RFLAGS.  Tunix uses
 * an iretq return path so the scheduler can resume contexts captured either by
 * SYSCALL or by a timer interrupt with one common process frame.
 *
 * The stack to switch to is per-processor and has to be found before there is
 * a stack to find it with, which is what GS is for: swapgs brings this
 * processor's block into reach without touching a register the caller owns.
 */
syscall_entry:
    swapgs
    mov qword ptr gs:[CPU_USER_RSP], rsp
    mov rsp, qword ptr gs:[CPU_KERNEL_RSP]
    test rsp, rsp
    jz syscall_fatal

    sub rsp, 144
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

    /* Match architectural SYSCALL clobber semantics for resumed contexts. */
    mov qword ptr [rsp + 104], rcx
    mov qword ptr [rsp + 112], r11
    mov qword ptr [rsp + 120], rcx
    mov qword ptr [rsp + 128], r11
    mov rax, qword ptr gs:[CPU_USER_RSP]
    mov qword ptr [rsp + 136], rax

    mov rdi, rsp
    call syscall_dispatch

    /* The frame may have been moved onto the kernel stack of a different
       process while the lock was held -- see syscall_dispatch. Follow it, and
       only then let go of the lock: everything touched from here on, the
       return addresses included, has to be on a stack no other processor can
       be standing on. */
    mov rsp, qword ptr gs:[CPU_KERNEL_RSP]
    sub rsp, 144
    call kernel_unlock_current

    /* Build an iretq frame below the saved process context. */
    mov r11, rsp
    push 0x1b
    push qword ptr [r11 + 136]
    push qword ptr [r11 + 128]
    push 0x23
    push qword ptr [r11 + 120]

    mov r15, qword ptr [r11 + 0]
    mov r14, qword ptr [r11 + 8]
    mov r13, qword ptr [r11 + 16]
    mov r12, qword ptr [r11 + 24]
    mov rbp, qword ptr [r11 + 32]
    mov rbx, qword ptr [r11 + 40]
    mov r9, qword ptr [r11 + 48]
    mov r8, qword ptr [r11 + 56]
    mov r10, qword ptr [r11 + 64]
    mov rdx, qword ptr [r11 + 72]
    mov rsi, qword ptr [r11 + 80]
    mov rdi, qword ptr [r11 + 88]
    mov rax, qword ptr [r11 + 96]
    mov rcx, qword ptr [r11 + 104]
    mov r11, qword ptr [r11 + 112]
    swapgs
    iretq

syscall_fatal:
    cli
1:
    hlt
    jmp 1b

process_enter_user:
    cli
    mov rax, rdx
    mov cr3, rax
    /* Before the segment loads, because loading GS clears its base and the
       block must be moved to the kernel side while it is still reachable. */
    swapgs
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
