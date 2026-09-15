    .section .text.head, "ax"
    .globl _start
_start:
    b       primary_entry
    .long   0
    .quad   0
    .quad   __image_size
    .quad   0xA
    .quad   0
    .quad   0
    .quad   0
    .ascii  "ARM\x64"
    .long   0

primary_entry:
    mov     x19, x0
    msr     daifset, #0xf
    adr     x20, _start

    bl      drop_to_el1

    adrp    x0, __bss_start
    add     x0, x0, :lo12:__bss_start
    adrp    x1, __bss_end
    add     x1, x1, :lo12:__bss_end
1:  cmp     x0, x1
    b.hs    2f
    str     xzr, [x0], #8
    b       1b
2:
    adrp    x0, boot_stack_top
    add     x0, x0, :lo12:boot_stack_top
    mov     sp, x0

    mov     x0, x20
    mov     x1, x19
    bl      aarch64_build_early_tables

    adrp    x0, boot_root
    add     x0, x0, :lo12:boot_root
    bl      enable_mmu

    ldr     x0, =virtual_entry
    br      x0

virtual_entry:
    ldr     x0, =boot_stack_top
    mov     sp, x0
    ldr     x0, =exception_vectors
    msr     vbar_el1, x0
    isb
    mov     x29, #0
    mov     x0, x19
    mov     x1, x20
    bl      aarch64_start
3:  wfi
    b       3b

    .globl  secondary_entry
secondary_entry:
    msr     daifset, #0xf
    bl      drop_to_el1
    adrp    x1, secondary_root
    ldr     x0, [x1, :lo12:secondary_root]
    bl      enable_mmu
    ldr     x0, =secondary_virtual
    br      x0

secondary_virtual:
    ldr     x1, =secondary_stack_top
    ldr     x0, [x1]
    mov     sp, x0
    ldr     x0, =exception_vectors
    msr     vbar_el1, x0
    isb
    mov     x29, #0
    ldr     x1, =secondary_index
    ldr     x0, [x1]
    bl      aarch64_secondary_start
4:  wfi
    b       4b

enable_mmu:
    movz    x1, #0x44, lsl #16
    movk    x1, #0x04ff
    msr     mair_el1, x1

    mrs     x2, id_aa64mmfr0_el1
    and     x2, x2, #0xf
    cmp     x2, #5
    b.ls    5f
    mov     x2, #5
5:  lsl     x2, x2, #32
    movz    x1, #0xb510, lsl #16
    movk    x1, #0x3511
    orr     x1, x1, x2
    msr     tcr_el1, x1

    msr     ttbr0_el1, x0
    msr     ttbr1_el1, x0
    isb
    tlbi    vmalle1
    dsb     sy
    isb

    movz    x1, #0x34d5, lsl #16
    movk    x1, #0xd81d
    msr     sctlr_el1, x1
    isb
    ret

drop_to_el1:
    mrs     x0, CurrentEL
    lsr     x0, x0, #2
    cmp     x0, #2
    b.ne    9f

    mrs     x0, cnthctl_el2
    orr     x0, x0, #3
    msr     cnthctl_el2, x0
    msr     cntvoff_el2, xzr

    mrs     x0, id_aa64pfr0_el1
    ubfx    x0, x0, #24, #4
    cbz     x0, 6f
    mrs     x0, ICC_SRE_EL2
    orr     x0, x0, #1
    msr     ICC_SRE_EL2, x0
    isb
6:
    mov     x0, #(1 << 31)
    msr     hcr_el2, x0
    msr     hstr_el2, xzr
    mov     x0, #0x33ff
    msr     cptr_el2, x0
    msr     vttbr_el2, xzr
    mrs     x0, midr_el1
    msr     vpidr_el2, x0
    mrs     x0, mpidr_el1
    msr     vmpidr_el2, x0

    movz    x0, #0x30d0, lsl #16
    movk    x0, #0x0800
    msr     sctlr_el1, x0

    mov     x0, #0x3c5
    msr     spsr_el2, x0
    msr     elr_el2, x30
    eret
9:
    movz    x0, #0x30d0, lsl #16
    movk    x0, #0x0800
    msr     sctlr_el1, x0
    isb
    ret

    .ltorg
