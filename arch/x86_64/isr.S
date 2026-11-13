.intel_syntax noprefix
.section .text
.global gdt_flush
.global tss_flush
.global idt_load
.extern isr_handler
.extern kernel_leave_from_isr
.extern kernel_exit_from_isr
.extern smp_flush_interrupt

.set CPU_KERNEL_RSP, 0

.set FRAME_BYTES, 184
.set FRAME_CS, 152

gdt_flush:
    lgdt [rdi]
    push 0x08
    lea rax, [rip + .reload_cs]
    push rax
    retfq
.reload_cs:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    ret

tss_flush:
    mov ax, 0x28
    ltr ax
    ret

idt_load:
    lidt [rdi]
    ret

.global isr0
isr0:
    cli
    push 0
    push 0
    jmp isr_common_stub

.global isr1
isr1:
    cli
    push 0
    push 1
    jmp isr_common_stub

.global isr2
isr2:
    cli
    push 0
    push 2
    jmp isr_common_stub

.global isr3
isr3:
    cli
    push 0
    push 3
    jmp isr_common_stub

.global isr4
isr4:
    cli
    push 0
    push 4
    jmp isr_common_stub

.global isr5
isr5:
    cli
    push 0
    push 5
    jmp isr_common_stub

.global isr6
isr6:
    cli
    push 0
    push 6
    jmp isr_common_stub

.global isr7
isr7:
    cli
    push 0
    push 7
    jmp isr_common_stub

.global isr8
isr8:
    cli
    push 8
    jmp isr_common_stub

.global isr9
isr9:
    cli
    push 0
    push 9
    jmp isr_common_stub

.global isr10
isr10:
    cli
    push 10
    jmp isr_common_stub

.global isr11
isr11:
    cli
    push 11
    jmp isr_common_stub

.global isr12
isr12:
    cli
    push 12
    jmp isr_common_stub

.global isr13
isr13:
    cli
    push 13
    jmp isr_common_stub

.global isr14
isr14:
    cli
    push 14
    jmp isr_common_stub

.global isr15
isr15:
    cli
    push 0
    push 15
    jmp isr_common_stub

.global isr16
isr16:
    cli
    push 0
    push 16
    jmp isr_common_stub

.global isr17
isr17:
    cli
    push 17
    jmp isr_common_stub

.global isr18
isr18:
    cli
    push 0
    push 18
    jmp isr_common_stub

.global isr19
isr19:
    cli
    push 0
    push 19
    jmp isr_common_stub

.global isr20
isr20:
    cli
    push 0
    push 20
    jmp isr_common_stub

.global isr21
isr21:
    cli
    push 0
    push 21
    jmp isr_common_stub

.global isr22
isr22:
    cli
    push 0
    push 22
    jmp isr_common_stub

.global isr23
isr23:
    cli
    push 0
    push 23
    jmp isr_common_stub

.global isr24
isr24:
    cli
    push 0
    push 24
    jmp isr_common_stub

.global isr25
isr25:
    cli
    push 0
    push 25
    jmp isr_common_stub

.global isr26
isr26:
    cli
    push 0
    push 26
    jmp isr_common_stub

.global isr27
isr27:
    cli
    push 0
    push 27
    jmp isr_common_stub

.global isr28
isr28:
    cli
    push 0
    push 28
    jmp isr_common_stub

.global isr29
isr29:
    cli
    push 0
    push 29
    jmp isr_common_stub

.global isr30
isr30:
    cli
    push 30
    jmp isr_common_stub

.global isr31
isr31:
    cli
    push 0
    push 31
    jmp isr_common_stub

isr_common_stub:

    cld
    test byte ptr [rsp + 24], 3
    jz 1f
    swapgs
1:
    push r15
    push r14
    push r13
    push r12
    push r11
    push r10
    push r9
    push r8
    push rbp
    push rdi
    push rsi
    push rdx
    push rcx
    push rbx
    push rax
    mov ax, ds
    push rax
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov rdi, rsp
    sub rsp, 8
    call isr_handler
    add rsp, 8

    test byte ptr [rsp + FRAME_CS], 3
    jz 3f
    mov rsp, qword ptr gs:[CPU_KERNEL_RSP]
    sub rsp, FRAME_BYTES
    sub rsp, 8
    call kernel_exit_from_isr
    add rsp, 8
    jmp 4f
3:
    sub rsp, 8
    call kernel_leave_from_isr
    add rsp, 8
4:
    pop rax
    mov ds, ax
    mov es, ax
    pop rax
    pop rbx
    pop rcx
    pop rdx
    pop rsi
    pop rdi
    pop rbp
    pop r8
    pop r9
    pop r10
    pop r11
    pop r12
    pop r13
    pop r14
    pop r15
    add rsp, 16
    test byte ptr [rsp + 8], 3
    jz 2f
    swapgs
2:
    iretq

.global irq0
irq0:
    cli
    push 0
    push 32
    jmp isr_common_stub

.global irq1
irq1:
    cli
    push 0
    push 33
    jmp isr_common_stub

.global irq12
irq12:
    cli
    push 0
    push 44
    jmp isr_common_stub

.global irq_lapic_timer
irq_lapic_timer:
    cli
    push 0
    push 0xF0
    jmp isr_common_stub

.global irq_reschedule
irq_reschedule:
    cli
    push 0
    push 0xF2
    jmp isr_common_stub

.global irq_invalidate_tlb
irq_invalidate_tlb:
    cli
    cld
    test byte ptr [rsp + 8], 3
    jz 1f
    swapgs
1:
    push rax
    push rcx
    push rdx
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    call smp_flush_interrupt
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rax
    test byte ptr [rsp + 8], 3
    jz 2f
    swapgs
2:
    iretq

.irp number, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,49,50,51,52,53,54,55,56,57,58,59,60,61,62,63,64,65,66,67,68,69,70,71,72,73,74,75,76,77,78,79,80,81,82,83,84,85,86,87,88,89,90,91,92,93,94,95,96,97,98,99,100,101,102,103,104,105,106,107,108,109,110,111,112,113,114,115,116,117,118,119,120,121,122,123,124,125,126,127
.global irq_device\number
irq_device\number:
    cli
    push 0
    push 0x40 + \number
    jmp isr_common_stub
.endr

.section .rodata
.global irq_device_stubs
irq_device_stubs:
.irp number, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,49,50,51,52,53,54,55,56,57,58,59,60,61,62,63,64,65,66,67,68,69,70,71,72,73,74,75,76,77,78,79,80,81,82,83,84,85,86,87,88,89,90,91,92,93,94,95,96,97,98,99,100,101,102,103,104,105,106,107,108,109,110,111,112,113,114,115,116,117,118,119,120,121,122,123,124,125,126,127
    .quad irq_device\number
.endr
.section .text
