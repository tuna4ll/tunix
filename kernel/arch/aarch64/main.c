#include <stdint.h>

#include "arch.h"

static const char *current_el_name(void) {
    switch ((sysreg_read("CurrentEL") >> 2) & 3) {
    case 1: return "EL1";
    case 2: return "EL2";
    case 3: return "EL3";
    default: return "EL0";
    }
}

void aarch64_sync_handler(uint64_t esr, uint64_t elr, uint64_t far) {
    kprintf("\n[aarch64] synchronous exception ESR=%lx ELR=%lx FAR=%lx\n", esr, elr, far);
}

void aarch64_fatal_handler(uint64_t esr, uint64_t elr) {
    kprintf("\n[aarch64] fatal exception ESR=%lx ELR=%lx -- halting\n", esr, elr);
}

void aarch64_irq_handler(void) {
    uint32_t intid = gic_acknowledge();
    if (intid == TIMER_PPI_INTID) {
        timer_tick();
        gic_eoi(intid);
        if ((timer_ticks() % 100) == 0)
            kprintf("[aarch64] tick %lu (%lu s)\n", timer_ticks(), timer_ticks() / 100);
        return;
    }
    if (intid < 1020) gic_eoi(intid);
}

void aarch64_main(uint64_t dtb) {
    uart_init();
    kprintf("\n=== Tunix aarch64 ===\n");
    kprintf("running at %s\n", current_el_name());

    uint64_t ram = 0;
    uint32_t cpus = 0;
    const void *dt = fdt_find((const void *)dtb);
    if (dt && fdt_probe(dt, &ram, &cpus) == 0)
        kprintf("device tree @ %p: %lu MiB RAM, %u CPU(s)\n", dt, ram >> 20, cpus);
    else
        kprintf("device tree: not found\n");

    mmu_init();
    kprintf("MMU enabled (identity map, MAIR/TCR set)\n");

    gic_init();
    kprintf("GICv3 initialised\n");

    timer_init();
    kprintf("generic timer armed at 100 Hz, enabling IRQs\n");

    __asm__ volatile("msr daifclr, #2" ::: "memory");   // unmask IRQ

    kprintf("running; waiting for timer interrupts...\n");
    for (;;) __asm__ volatile("wfi");
}
