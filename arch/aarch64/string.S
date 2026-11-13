    .text

    .globl memset
    .type memset, %function
memset:
    mov x3, x0
    and x1, x1, #0xff
    mov x4, #0x0101010101010101
    mul x4, x4, x1
    cmp x2, #16
    b.lo 2f
1:  stp x4, x4, [x3], #16
    sub x2, x2, #16
    cmp x2, #16
    b.hs 1b
2:  cbz x2, 4f
3:  strb w1, [x3], #1
    subs x2, x2, #1
    b.ne 3b
4:  ret
    .size memset, . - memset

    .globl memcpy
    .type memcpy, %function
memcpy:
    mov x3, x0
    cmp x2, #16
    b.lo 2f
1:  ldp x4, x5, [x1], #16
    stp x4, x5, [x3], #16
    sub x2, x2, #16
    cmp x2, #16
    b.hs 1b
2:  cbz x2, 4f
3:  ldrb w4, [x1], #1
    strb w4, [x3], #1
    subs x2, x2, #1
    b.ne 3b
4:  ret
    .size memcpy, . - memcpy

    .globl memmove
    .type memmove, %function
memmove:
    cmp x0, x1
    b.ls memcpy
    add x4, x1, x2
    cmp x0, x4
    b.hs memcpy
    add x3, x0, x2
    mov x1, x4
    cmp x2, #16
    b.lo 2f
1:  ldp x5, x6, [x1, #-16]!
    stp x5, x6, [x3, #-16]!
    sub x2, x2, #16
    cmp x2, #16
    b.hs 1b
2:  cbz x2, 4f
3:  ldrb w5, [x1, #-1]!
    strb w5, [x3, #-1]!
    subs x2, x2, #1
    b.ne 3b
4:  ret
    .size memmove, . - memmove
