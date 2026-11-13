.intel_syntax noprefix
.section .text

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

    mov eax, cr4
    or eax, (1 << 5) | (1 << 9) | (1 << 10)
    mov cr4, eax

    mov eax, [AT(trampoline_cr3)]
    mov cr3, eax

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
    mov eax, AT(smp_trampoline_data)
    mov rsp, [rax + 8]
    mov rdi, [rax + 24]
    mov rbx, [rax + 16]
    xor rbp, rbp
    jmp rbx

.align 8
trampoline_gdt:
    .quad 0x0000000000000000
    .quad 0x00CF9A000000FFFF
    .quad 0x00CF92000000FFFF
    .quad 0x00AF9A000000FFFF
    .quad 0x00AF92000000FFFF
trampoline_gdt_end:

trampoline_gdtr:
    .word trampoline_gdt_end - trampoline_gdt - 1
    .long AT(trampoline_gdt)

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
