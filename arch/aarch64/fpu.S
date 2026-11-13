    .arch   armv8-a+fp+simd
    .section .text

    .globl process_enable_extended_fpu
process_enable_extended_fpu:
    mrs     x0, cpacr_el1
    orr     x0, x0, #(3 << 20)
    msr     cpacr_el1, x0
    isb
    ret

    .globl arch_fpu_save
arch_fpu_save:
    stp     q0, q1,   [x0, #16 * 0]
    stp     q2, q3,   [x0, #16 * 2]
    stp     q4, q5,   [x0, #16 * 4]
    stp     q6, q7,   [x0, #16 * 6]
    stp     q8, q9,   [x0, #16 * 8]
    stp     q10, q11, [x0, #16 * 10]
    stp     q12, q13, [x0, #16 * 12]
    stp     q14, q15, [x0, #16 * 14]
    stp     q16, q17, [x0, #16 * 16]
    stp     q18, q19, [x0, #16 * 18]
    stp     q20, q21, [x0, #16 * 20]
    stp     q22, q23, [x0, #16 * 22]
    stp     q24, q25, [x0, #16 * 24]
    stp     q26, q27, [x0, #16 * 26]
    stp     q28, q29, [x0, #16 * 28]
    stp     q30, q31, [x0, #16 * 30]
    mrs     x1, fpsr
    mrs     x2, fpcr
    add     x3, x0, #16 * 32
    stp     x1, x2,   [x3]
    ret

    .globl arch_fpu_restore
arch_fpu_restore:
    ldp     q0, q1,   [x0, #16 * 0]
    ldp     q2, q3,   [x0, #16 * 2]
    ldp     q4, q5,   [x0, #16 * 4]
    ldp     q6, q7,   [x0, #16 * 6]
    ldp     q8, q9,   [x0, #16 * 8]
    ldp     q10, q11, [x0, #16 * 10]
    ldp     q12, q13, [x0, #16 * 12]
    ldp     q14, q15, [x0, #16 * 14]
    ldp     q16, q17, [x0, #16 * 16]
    ldp     q18, q19, [x0, #16 * 18]
    ldp     q20, q21, [x0, #16 * 20]
    ldp     q22, q23, [x0, #16 * 22]
    ldp     q24, q25, [x0, #16 * 24]
    ldp     q26, q27, [x0, #16 * 26]
    ldp     q28, q29, [x0, #16 * 28]
    ldp     q30, q31, [x0, #16 * 30]
    add     x3, x0, #16 * 32
    ldp     x1, x2,   [x3]
    msr     fpsr, x1
    msr     fpcr, x2
    ret

    .globl arch_fpu_init
arch_fpu_init:
    mov     x1, #528
1:  strb    wzr, [x0], #1
    subs    x1, x1, #1
    b.ne    1b
    ret
