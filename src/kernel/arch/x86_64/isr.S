.intel_syntax noprefix
.section .text
.global gdt_flush
.global tss_flush
.global idt_load
.extern isr_handler
.extern kernel_unlock_current
.extern smp_flush_interrupt

/* struct cpu, as percpu.h lays it out. */
.set CPU_KERNEL_RSP, 0
/* struct interrupt_frame, as interrupt.h lays it out. */
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

/*
 * Whether GS has to be swapped is decided by the privilege the frame came
 * from, and on the way out by the privilege it is going to -- which are not
 * always the same frame. An interrupt taken in the idle loop arrives at CPL 0
 * with the block already in GS, and leaves through a process's frame at CPL 3,
 * so it swaps on the way out and not on the way in. Reading each end's own CS
 * is what makes that work without a special case.
 */
isr_common_stub:
    /*
     * The direction flag belongs to whoever was interrupted, and the C below
     * assumes it is clear -- the ABI says so, and the compiler acts on it: a
     * struct assignment or a memset is a `rep movs`/`rep stos`, which walks
     * backwards when DF is set and writes over whatever lies *before* the
     * destination. User code sets it legitimately (musl's memmove does, for an
     * overlapping copy) and an interrupt landing in that window used to hand
     * the flag straight to the kernel. The syscall path never had the problem:
     * FMASK clears DF on the way in. This is the interrupt path's FMASK.
     */
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
    /* isr_handler may have moved the frame onto the kernel stack of a
       different process; follow it. cs sits at offset 152 of the frame, and a
       user cs is what says the move happened. */
    test byte ptr [rsp + FRAME_CS], 3
    jz 3f
    mov rsp, qword ptr gs:[CPU_KERNEL_RSP]
    sub rsp, FRAME_BYTES
3:
    /* Only now: isr_handler came back with the lock still held, because its
       own return address was on the stack this may have just moved off. */
    sub rsp, 8
    call kernel_unlock_current
    add rsp, 8
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

/* Legacy 8259 PIC IRQ0 timer, remapped to IDT vector 0x20. */
.global irq0
irq0:
    cli
    push 0
    push 32
    jmp isr_common_stub

/* Legacy 8259 PIC IRQ1, remapped to IDT vector 0x21. */
.global irq1
irq1:
    cli
    push 0
    push 33
    jmp isr_common_stub

/* Legacy 8259 PIC IRQ12 PS/2 auxiliary device, remapped to IDT vector 0x2c. */
.global irq12
irq12:
    cli
    push 0
    push 44
    jmp isr_common_stub

/* The ACPI system control interrupt, on IDT vector 0x30. Where the firmware
   actually raises it is a global interrupt number the FADT names, so unlike
   the legacy lines above it there is no fixed IRQ to call this. */
.global irq_sci
irq_sci:
    cli
    push 0
    push 0x30
    jmp isr_common_stub

/* The local APIC timer, which is what preempts every processor the PIT is not
   wired to. */
.global irq_lapic_timer
irq_lapic_timer:
    cli
    push 0
    push 0xF0
    jmp isr_common_stub

/*
 * One processor asking the others to drop their cached translations.
 *
 * A stub of its own rather than a case in the common one, because it must not
 * go near the kernel lock: the processor that sent it is holding the lock and
 * waiting for this answer, so taking the lock here would be waiting for the
 * waiter. Only the caller-saved registers are spilled -- the C it calls
 * preserves the rest.
 */
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
