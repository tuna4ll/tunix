    .section .text.boot
    .globl _start
    // arm64 Linux Image header, so QEMU passes the DTB in x0.
_start:
    b       real_start
    .long   0
    .quad   0                       // text_offset
    .quad   __image_end - _start    // image_size
    .quad   0                       // flags
    .quad   0
    .quad   0
    .quad   0
    .ascii  "ARM\x64"               // magic
    .long   0

    // The MMU is off here, so every symbol is reached PC-relatively: adrp
    // yields the physical address while we run below our virtual link base.
real_start:
    // Firmware hands us the DTB in x0; keep it safe.
    mov     x19, x0
    msr     daifset, #0xf

    adr     x9, _start                      // where we were actually loaded
    movz    x10, #0x4000, lsl #16           // where we must run: 0x40000000
    subs    x12, x10, x9                    // delta to apply
    b.eq    2f

    adrp    x11, __bss_start
    add     x11, x11, :lo12:__bss_start
    sub     x11, x11, x9                    // bytes the loader placed

    mov     x13, x10
    mov     x14, x9
1:  ldr     x15, [x14], #8
    str     x15, [x13], #8
    subs    x11, x11, #8
    b.hi    1b

    adr     x15, 2f
    add     x15, x15, x12
    br      x15                             // continue from the relocated copy

2:
    mrs     x0, CurrentEL
    lsr     x0, x0, #2
    cmp     x0, #2
    b.ne    5f

    // EL2: let EL1 use the physical timer and GIC system registers.
    mrs     x0, cnthctl_el2
    orr     x0, x0, #3              // EL1PCTEN | EL1PCEN
    msr     cnthctl_el2, x0
    msr     cntvoff_el2, xzr
    mov     x0, #0xf                // SRE | DFB | DIB | Enable
    msr     ICC_SRE_EL2, x0
    isb

    mov     x0, #(1 << 31)          // HCR_EL2.RW = EL1 is AArch64
    msr     hcr_el2, x0
    mov     x0, #0x3c5              // EL1h, DAIF masked
    msr     spsr_el2, x0
    adr     x0, 5f
    msr     elr_el2, x0
    eret

5:
    adrp    x0, __stack_top
    add     x0, x0, :lo12:__stack_top
    mov     sp, x0

    adrp    x0, __bss_start
    add     x0, x0, :lo12:__bss_start
    adrp    x1, __bss_end
    add     x1, x1, :lo12:__bss_end
6:  cmp     x0, x1
    b.hs    7f
    str     xzr, [x0], #8
    b       6b
7:
    // A physical vector base, so an early fault still reports itself.
    adrp    x0, __vectors
    add     x0, x0, :lo12:__vectors
    msr     vbar_el1, x0
    isb

    bl      uart_init
    bl      mmu_init                // identity in TTBR0, kernel map in TTBR1

    ldr     x0, =virt_entry         // the first absolute address we may use
    br      x0

    // From here the kernel executes at its virtual address.
virt_entry:
    ldr     x0, =__stack_top
    mov     sp, x0

    ldr     x0, =__vectors
    msr     vbar_el1, x0
    isb

    bl      arch_va_online
    mov     x0, x19                 // pass the DTB physical address to C
    bl      aarch64_main
8:  wfi
    b       8b
