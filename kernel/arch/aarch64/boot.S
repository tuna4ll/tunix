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

real_start:
    // Firmware hands us the DTB in x0; keep it safe.
    mov     x19, x0
    msr     daifset, #0xf

    // The loader may place the Image anywhere; relocate to our link address.
    adr     x9, _start              // runtime base
    ldr     x10, =_start            // link base
    ldr     x11, =__bss_start
    sub     x11, x11, x10           // loaded byte count
    subs    x12, x9, x10            // delta = runtime - link
    b.eq    2f
    mov     x13, x10
1:  cbz     x11, 3f
    ldr     x14, [x9], #8
    str     x14, [x13], #8
    sub     x11, x11, #8
    b       1b
3:  ldr     x15, =2f
    br      x15                     // continue from the relocated copy

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
    ldr     x0, =__stack_top
    mov     sp, x0

    ldr     x0, =__bss_start
    ldr     x1, =__bss_end
6:  cmp     x0, x1
    b.hs    7f
    str     xzr, [x0], #8
    b       6b
7:
    ldr     x0, =__vectors
    msr     vbar_el1, x0
    isb

    mov     x0, x19                 // pass DTB pointer to C
    bl      aarch64_main
8:  wfi
    b       8b
