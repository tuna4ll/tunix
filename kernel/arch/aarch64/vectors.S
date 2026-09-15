#define FRAME_SIZE 288
#define FRAME_SPSR 256
#define CPU_KERNEL_RSP 0

    .macro SAVE_FRAME
    sub     sp, sp, #FRAME_SIZE
    stp     x0, x1,   [sp, #16 * 0]
    stp     x2, x3,   [sp, #16 * 1]
    stp     x4, x5,   [sp, #16 * 2]
    stp     x6, x7,   [sp, #16 * 3]
    stp     x8, x9,   [sp, #16 * 4]
    stp     x10, x11, [sp, #16 * 5]
    stp     x12, x13, [sp, #16 * 6]
    stp     x14, x15, [sp, #16 * 7]
    stp     x16, x17, [sp, #16 * 8]
    stp     x18, x19, [sp, #16 * 9]
    stp     x20, x21, [sp, #16 * 10]
    stp     x22, x23, [sp, #16 * 11]
    stp     x24, x25, [sp, #16 * 12]
    stp     x26, x27, [sp, #16 * 13]
    stp     x28, x29, [sp, #16 * 14]
    mrs     x9, elr_el1
    stp     x30, x9,  [sp, #16 * 15]
    mrs     x10, spsr_el1
    mrs     x9, sp_el0
    stp     x10, x9,  [sp, #16 * 16]
    stp     xzr, xzr, [sp, #16 * 17]
    .endm

    .macro RESTORE_FRAME
    ldp     x10, x9,  [sp, #16 * 16]
    msr     spsr_el1, x10
    msr     sp_el0, x9
    ldp     x30, x9,  [sp, #16 * 15]
    msr     elr_el1, x9
    ldp     x0, x1,   [sp, #16 * 0]
    ldp     x2, x3,   [sp, #16 * 1]
    ldp     x4, x5,   [sp, #16 * 2]
    ldp     x6, x7,   [sp, #16 * 3]
    ldp     x8, x9,   [sp, #16 * 4]
    ldp     x10, x11, [sp, #16 * 5]
    ldp     x12, x13, [sp, #16 * 6]
    ldp     x14, x15, [sp, #16 * 7]
    ldp     x16, x17, [sp, #16 * 8]
    ldp     x18, x19, [sp, #16 * 9]
    ldp     x20, x21, [sp, #16 * 10]
    ldp     x22, x23, [sp, #16 * 11]
    ldp     x24, x25, [sp, #16 * 12]
    ldp     x26, x27, [sp, #16 * 13]
    ldp     x28, x29, [sp, #16 * 14]
    add     sp, sp, #FRAME_SIZE
    .endm

    .macro VECTOR label
    .align  7
    b       \label
    .endm

    .section .text.vectors, "ax"
    .align  11
    .globl  exception_vectors
exception_vectors:
    VECTOR  unexpected_el1t
    VECTOR  unexpected_el1t
    VECTOR  unexpected_el1t
    VECTOR  unexpected_el1t
    VECTOR  el1_sync
    VECTOR  el1_irq
    VECTOR  unexpected_fiq
    VECTOR  unexpected_error
    VECTOR  el0_sync
    VECTOR  el0_irq
    VECTOR  unexpected_fiq
    VECTOR  unexpected_error
    VECTOR  unexpected_el0_32
    VECTOR  unexpected_el0_32
    VECTOR  unexpected_el0_32
    VECTOR  unexpected_el0_32

el1_sync:
    SAVE_FRAME
    mov     x0, sp
    bl      aarch64_el1_sync
    RESTORE_FRAME
    eret

el1_irq:
el0_irq:
    SAVE_FRAME
    mov     x0, sp
    bl      aarch64_irq
    ldr     x9, [sp, #FRAME_SPSR]
    tst     x9, #0xf
    b.ne    1f
    mrs     x9, tpidr_el1
    ldr     x10, [x9, #CPU_KERNEL_RSP]
    cbz     x10, 1f
    sub     sp, x10, #FRAME_SIZE
1:  bl      kernel_unlock_from_isr
    RESTORE_FRAME
    eret

el0_sync:
    SAVE_FRAME
    mov     x0, sp
    bl      aarch64_el0_sync
    mov     x19, x0
    mrs     x9, tpidr_el1
    ldr     x10, [x9, #CPU_KERNEL_RSP]
    cbz     x10, 1f
    sub     sp, x10, #FRAME_SIZE
1:  cmp     x19, #1
    b.ne    2f
    bl      kernel_unlock_current
    b       3f
2:  bl      kernel_unlock_from_isr
3:  RESTORE_FRAME
    eret

unexpected_el1t:
    SAVE_FRAME
    mov     x0, sp
    mov     x1, #0
    bl      aarch64_unexpected
    b       .

unexpected_fiq:
    SAVE_FRAME
    mov     x0, sp
    mov     x1, #1
    bl      aarch64_unexpected
    b       .

unexpected_error:
    SAVE_FRAME
    mov     x0, sp
    mov     x1, #2
    bl      aarch64_unexpected
    b       .

unexpected_el0_32:
    SAVE_FRAME
    mov     x0, sp
    mov     x1, #3
    bl      aarch64_unexpected
    b       .
