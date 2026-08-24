.intel_syntax noprefix
.section .text

/*
 * Where an application processor starts.
 *
 * A processor woken by a startup IPI begins in 16-bit real mode at a physical
 * address the IPI names, which must be below 1 MiB and page-aligned -- so this
 * is copied to TRAMPOLINE_BASE and run from there, not from where it is
 * linked. Nothing here can use a link-time address: every reference is written
 * as an offset into the blob plus the address it was copied to.
 *
 * It has three jobs and no room to be clever about any of them: reach 32-bit
 * protected mode, turn on PAE and long mode with the kernel's own page tables,
 * and jump to C. Until the last of those there is no IDT, so any mistake is a
 * triple fault and a silent reboot rather than a message.
 *
 * The 2 MiB identity mapping this runs under is put in place by smp.c before
 * the first IPI and taken out again once every processor is up; the kernel
 * proper is mapped high and needs no such thing.
 */
#define TRAMPOLINE_BASE 0x8000
#define AT(label) ((label) - smp_trampoline_start + TRAMPOLINE_BASE)

.global smp_trampoline_start
.global smp_trampoline_end
.global smp_trampoline_data

.code16
.align 16
smp_trampoline_start:
    cli
    cld
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    lgdt [AT(trampoline_gdtr)]
    mov eax, cr0
    or eax, 1
    mov cr0, eax
    ljmp 0x08, AT(protected_mode)

.code32
protected_mode:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax

    /* PAE, and the two bits that make fxsave64 legal -- the kernel saves
       process FPU state with it on every switch. */
    mov eax, cr4
    or eax, (1 << 5) | (1 << 9) | (1 << 10)
    mov cr4, eax

    mov eax, [AT(trampoline_cr3)]
    mov cr3, eax

    /* Long mode, no-execute and the syscall instruction. NXE has to be on
       before paging is: the kernel's page tables already have bit 63 set on
       every non-executable page, and to a processor without NXE that bit is
       reserved, so the first fetch would fault. */
    mov ecx, 0xC0000080
    rdmsr
    or eax, (1 << 0) | (1 << 8) | (1 << 11)
    wrmsr

    mov eax, cr0
    and eax, ~(1 << 2)
    or eax, (1 << 0) | (1 << 1) | (1 << 5) | (1 << 31)
    mov cr0, eax

    ljmp 0x18, AT(long_mode)

.code64
long_mode:
    /* A 32-bit immediate rather than a rip-relative load: the blob is running
       at an address the linker never saw. */
    mov eax, AT(smp_trampoline_data)
    mov rsp, [rax + 8]
    mov rdi, [rax + 24]
    mov rbx, [rax + 16]
    xor rbp, rbp
    jmp rbx

.align 8
trampoline_gdt:
    .quad 0x0000000000000000
    .quad 0x00CF9A000000FFFF   /* 0x08  32-bit code */
    .quad 0x00CF92000000FFFF   /* 0x10  32-bit data */
    .quad 0x00AF9A000000FFFF   /* 0x18  64-bit code */
    .quad 0x00AF92000000FFFF   /* 0x20  64-bit data */
trampoline_gdt_end:

trampoline_gdtr:
    .word trampoline_gdt_end - trampoline_gdt - 1
    .long AT(trampoline_gdt)

/* Filled in by smp.c before each startup IPI. One processor is started at a
   time, so one block is enough. */
.align 8
smp_trampoline_data:
trampoline_cr3:
    .quad 0
trampoline_stack:
    .quad 0
trampoline_entry:
    .quad 0
trampoline_index:
    .quad 0

smp_trampoline_end:
