#include <stdint.h>

#include "arch.h"

extern char kernel_start[];

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

static void address_space_selftest(void) {
    uint64_t space_a = vmm_create_space();
    uint64_t space_b = vmm_create_space();
    uint64_t frame_a = (uint64_t)pmm_alloc_page();
    uint64_t frame_b = (uint64_t)pmm_alloc_page();
    uint64_t user_va = 0x0000000000400000UL;

    if (!space_a || !space_b || !frame_a || !frame_b ||
        vmm_map(space_a, user_va, frame_a, VMM_WRITE | VMM_USER) != 0 ||
        vmm_map(space_b, user_va, frame_b, VMM_WRITE | VMM_USER) != 0) {
        kprintf("address spaces: setup failed\n");
        return;
    }

    // Leaving the boot identity map behind: TTBR0 is user memory from now on.
    vmm_switch_space(space_a);
    kprintf("address spaces: identity map dropped, TTBR0 = %p\n",
            (void *)sysreg_read("ttbr0_el1"));

    volatile uint64_t *slot = (volatile uint64_t *)user_va;
    *slot = 0xAAAA;
    vmm_switch_space(space_b);
    *slot = 0xBBBB;
    uint64_t seen_b = *slot;
    vmm_switch_space(space_a);
    uint64_t seen_a = *slot;

    kprintf("VMM: VA %p reads %lx in A and %lx in B, isolation %s\n",
            (void *)user_va, seen_a, seen_b,
            (seen_a == 0xAAAA && seen_b == 0xBBBB) ? "OK" : "BAD");

    uint64_t before = pmm_free_pages();
    vmm_destroy_space(space_b);
    kprintf("VMM: destroying a space reclaimed %lu page-table frames\n",
            pmm_free_pages() - before);
}

void aarch64_main(uint64_t dtb_phys) {
    uart_init();
    kprintf("\n=== Tunix aarch64 ===\n");
    kprintf("running at %s, kernel at %p (higher half)\n", current_el_name(),
            (void *)kernel_start);

    uint64_t ram_base = KERNEL_PHYS_BASE, ram = 0;
    uint32_t cpus = 0;
    const void *dt = fdt_find((const void *)phys_to_virt(dtb_phys));
    if (dt && fdt_probe(dt, &ram_base, &ram, &cpus) == 0)
        kprintf("device tree @ %p: %lu MiB RAM @ %lx, %u CPU(s)\n",
                (void *)virt_to_phys((uint64_t)dt), ram >> 20, ram_base, cpus);
    else
        kprintf("device tree: not found\n");

    uint64_t dtb_size = dt ? 0x100000UL : 0;
    pmm_init(ram_base, ram ? ram : 0x20000000UL,
             dt ? virt_to_phys((uint64_t)dt) : 0, dtb_size);
    kprintf("PMM: %lu free frames (%lu MiB)\n", pmm_free_pages(),
            (pmm_free_pages() * 4096) >> 20);

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

    address_space_selftest();

    gic_init();
    kprintf("GICv3 initialised\n");

    timer_init();
    kprintf("generic timer armed at 100 Hz, enabling IRQs\n");

    __asm__ volatile("msr daifclr, #2" ::: "memory");   // unmask IRQ

    kprintf("running; waiting for timer interrupts...\n");
    for (;;) __asm__ volatile("wfi");
}
