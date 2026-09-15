    .section .text
    .globl  cpu_enter_idle
    .globl  cpu_idle_park

cpu_enter_idle:
    msr     daifset, #2
    mov     sp, x0
    mov     x29, #0
    bl      kernel_unlock_current
    b       idle_loop

cpu_idle_park:
    msr     daifset, #2
    mov     sp, x0
    mov     x29, #0

idle_loop:
    wfi
    msr     daifclr, #2
    msr     daifset, #2
    b       idle_loop

    .globl  psci_smc
psci_smc:
    smc     #0
    ret

    .globl  psci_hvc
psci_hvc:
    hvc     #0
    ret
