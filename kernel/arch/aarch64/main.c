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

    uint64_t ram_base = 0x40000000, ram = 0;
    uint32_t cpus = 0;
    const void *dt = fdt_find((const void *)dtb);
    if (dt && fdt_probe(dt, &ram_base, &ram, &cpus) == 0)
        kprintf("device tree @ %p: %lu MiB RAM @ %lx, %u CPU(s)\n", dt, ram >> 20,
                ram_base, cpus);
    else
        kprintf("device tree: not found\n");

    mmu_init();
    kprintf("MMU enabled (identity map, MAIR/TCR set)\n");

    uint64_t dtb_size = dt ? 0x100000UL : 0;
    pmm_init(ram_base, ram ? ram : 0x20000000UL, (uint64_t)dt, dtb_size);
    kprintf("PMM: %lu free frames (%lu MiB)\n", pmm_free_pages(),
            (pmm_free_pages() * 4096) >> 20);

    uint64_t va = 0x0000008000000000UL;             // a fresh, unmapped region
    uint64_t frame = (uint64_t)pmm_alloc_page();
    if (frame && vmm_map_page(va, frame, 1) == 0) {
        volatile uint64_t *p = (volatile uint64_t *)va;
        p[0] = 0xC0DE1234ABCD5678UL;
        p[1] = frame;
        int ok = (p[0] == 0xC0DE1234ABCD5678UL) && (p[1] == frame);
        kprintf("VMM: mapped %p -> %p, readback %s\n", (void *)va, (void *)frame,
                ok ? "OK" : "BAD");
        vmm_unmap_page(va);
        kprintf("VMM: unmapped %p\n", (void *)va);
    } else {
        kprintf("VMM: mapping test failed\n");
    }

    heap_init();
    uint64_t heap_before = heap_free_bytes();
    void *a = kmalloc(64), *b = kmalloc(4096), *c = kmalloc(32);
    int corrupt = 0;
    for (int i = 0; i < 4096; i++) ((uint8_t *)b)[i] = (uint8_t)i;
    for (int i = 0; i < 4096; i++) corrupt |= ((uint8_t *)b)[i] != (uint8_t)i;
    kfree(b);
    kfree(a);
    kfree(c);
    for (int r = 0; r < 2000; r++) {
        void *t = kmalloc(128 + (r & 511));
        if (!t) { corrupt = 1; break; }
        kfree(t);
    }
    uint64_t heap_after = heap_free_bytes();
    kprintf("heap: %lu KiB, alloc/free stress %s, reclaimed %s\n", heap_before >> 10,
            corrupt ? "FAILED" : "OK", heap_after == heap_before ? "fully" : "partly");

    gic_init();
    kprintf("GICv3 initialised\n");

    timer_init();
    kprintf("generic timer armed at 100 Hz, enabling IRQs\n");

    __asm__ volatile("msr daifclr, #2" ::: "memory");   // unmask IRQ

    kprintf("running; waiting for timer interrupts...\n");
    for (;;) __asm__ volatile("wfi");
}
