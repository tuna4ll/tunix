// Where Linux loaders enter vmlinuz: the 32-bit entry at the start of the payload
// (code32_start) and the 64-bit entry 0x200 after it, both with boot_params in esi.
// The 32-bit one maps the first 4 GiB and switches to long mode; from there on both
// run stub.c, which builds the kernel's page tables and enters it.

.intel_syntax noprefix
.section .head, "ax"

.code32
.global startup_32
startup_32:
    cli
    cld
    // Four bytes of boot_params' scratch field are the stack the protocol allows for
    // finding out where we were loaded.
    lea esp, [esi + 0x1e8]
    call 1f
1:
    pop ebp
    sub ebp, 1b - startup_32
    jmp startup_32_body

    .org 0x200
.code64
.global startup_64
startup_64:
    cli
    cld
    lea rdi, [rip + _stub_bss_start]
    lea rcx, [rip + _stub_bss_end]
    sub rcx, rdi
    xor eax, eax
    rep stosb
    lea rsp, [rip + stub_stack_top]
    mov rdi, rsi
    call stub_linux_main
2:
    hlt
    jmp 2b

.code32
startup_32_body:
    lea edi, [ebp + _stub_bss_offset]
    mov ecx, _stub_bss_size
    xor eax, eax
    rep stosb
    lea esp, [ebp + _stub_stack_top_offset]

    // Identity map of the first 4 GiB in 2 MiB pages: PML4, PDPT, four directories.
    lea edi, [ebp + _boot_tables_offset]
    lea eax, [edi + 0x1003]
    mov dword ptr [edi], eax
    lea ebx, [edi + 0x1000]
    lea eax, [edi + 0x2003]
    mov ecx, 4
1:
    mov dword ptr [ebx], eax
    add eax, 0x1000
    add ebx, 8
    loop 1b
    lea ebx, [edi + 0x2000]
    mov eax, 0x83
    mov ecx, 2048
2:
    mov dword ptr [ebx], eax
    add eax, 0x200000
    add ebx, 8
    loop 2b

    mov eax, cr4
    or eax, 0x20                        // PAE
    mov cr4, eax
    mov cr3, edi

    // EFER: long mode, and no-execute when the processor has it.
    xor edi, edi
    mov eax, 0x80000000
    cpuid
    cmp eax, 0x80000001
    jb 3f
    mov eax, 0x80000001
    cpuid
    mov edi, edx
3:
    mov ecx, 0xc0000080
    rdmsr
    or eax, 0x100
    test edi, 1 << 20
    jz 4f
    or eax, 0x800
4:
    wrmsr

    mov eax, cr0
    or eax, 0x80010001                  // paging, write protect, protected mode
    mov cr0, eax

    lea eax, [ebp + _stub_gdt_offset]
    mov dword ptr [ebp + _stub_gdt_pointer_offset + 2], eax
    mov dword ptr [ebp + _stub_gdt_pointer_offset + 6], 0
    lgdt [ebp + _stub_gdt_pointer_offset]
    lea eax, [ebp + _from_32_offset]
    push 0x08
    push eax
    retf

.code64
.global from_32
from_32:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax
    mov edi, esi
    lea rsp, [rip + stub_stack_top]
    call stub_linux_main
1:
    hlt
    jmp 1b

// stub_enter_kernel(cr3, entry, handoff, no_execute): loads the stub's GDT and the
// kernel's page tables and jumps to the kernel with the handoff in rdi.
.text
.global stub_enter_kernel
stub_enter_kernel:
    cli
    mov r8, rdx
    lea rax, [rip + stub_gdt]
    mov qword ptr [rip + stub_gdt_pointer + 2], rax
    lgdt [rip + stub_gdt_pointer]
    test rcx, rcx
    jz 1f
    mov ecx, 0xc0000080
    rdmsr
    or eax, 0x800
    wrmsr
1:
    mov rax, cr0
    or rax, 0x10000
    mov cr0, rax
    mov cr3, rdi
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax
    push 0x08
    lea rax, [rip + 2f]
    push rax
    .byte 0x48, 0xcb                    // lretq
2:
    mov rdi, r8
    jmp rsi

.data
.balign 16
.global stub_gdt
stub_gdt:
    .quad 0
    .quad 0x00af9a000000ffff            // 0x08: 64-bit code, the kernel's own selector
    .quad 0x00cf92000000ffff            // 0x10: data
.global stub_gdt_pointer
stub_gdt_pointer:
    .word 23
    .quad 0

// What the kernel is, from its symbols; absolute, whatever the load address.
.global stub_kernel_info
stub_kernel_info:
    .quad stub_kernel_entry
    .quad kernel_image_start
    .quad kernel_reserve_end

.bss
.balign 4096
.global boot_tables
boot_tables:
    .skip 6 * 4096
