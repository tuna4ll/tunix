#include <stdint.h>

#include "arch.h"

extern char kernel_start[];
extern char user_elf_start[];
extern char user_elf_end[];

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
        if ((timer_ticks() % 200) == 0)
            kprintf("[aarch64] alive at %lu s, task %d running, heap %lu KiB free\n",
                    timer_ticks() / 100, sched_current_id(), heap_free_bytes() >> 10);
        sched_tick();                   // preempt whoever was running
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

static void worker_task(void *argument) {
    uint64_t id = (uint64_t)argument;
    for (int round = 0; round < 3; round++) {
        kprintf("[task %lu] round %d at tick %lu\n", id, round, timer_ticks());
        uint64_t until = timer_ticks() + 15 + id * 5;
        while (timer_ticks() < until) {         // preemption moves us aside here
        }
    }
    kprintf("[task %lu] finished\n", id);
}

static void user_task(void *argument) {
    (void)argument;
    uint64_t space = vmm_create_space();
    uint64_t stack_pa = (uint64_t)pmm_alloc_page();
    uint64_t stack_va = 0x0000000000500000UL;
    uint64_t length = (uint64_t)(user_elf_end - user_elf_start);
    uint64_t entry = 0;

    if (!space || !stack_pa ||
        elf_load_image(space, user_elf_start, length, &entry) != 0 ||
        vmm_map(space, stack_va, stack_pa, VMM_USER | VMM_WRITE) != 0) {
        kprintf("usermode: could not load the %lu byte ELF\n", length);
        return;
    }

    sched_set_space(space);
    kprintf("[task %d] loaded a %lu byte ELF, entering EL0 at %p\n",
            sched_current_id(), length, (void *)entry);
    aarch64_enter_user(entry, stack_va + 4096);
    kprintf("[task %d] back at EL1\n", sched_current_id());
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

    if (virtio_mmio_probe() == 0)
        kprintf("virtio-mmio: no devices attached\n");

    int blk = virtio_blk_init();
    if (blk == 0) {
        kprintf("virtio-blk: %lu sectors (%lu MiB)\n", virtio_blk_capacity(),
                (virtio_blk_capacity() * 512) >> 20);
        static uint8_t sector[512];
        if (virtio_blk_read(0, sector) == 0)
            kprintf("virtio-blk: sector 0 reads %x %x %x %x %x %x %x %x\n",
                    sector[0], sector[1], sector[2], sector[3],
                    sector[4], sector[5], sector[6], sector[7]);
        else
            kprintf("virtio-blk: reading sector 0 failed\n");
    } else if (blk != -1) {
        kprintf("virtio-blk: initialisation failed (%d)\n", blk);
    }

    gic_init();
    kprintf("GICv3 initialised\n");

    timer_init();
    kprintf("generic timer armed at 100 Hz, enabling IRQs\n");

    sched_init();
    sched_create("worker-1", worker_task, (void *)1);
    sched_create("worker-2", worker_task, (void *)2);
    sched_create("usertest", user_task, NULL);
    kprintf("scheduler: 3 tasks queued behind the idle task\n");

    __asm__ volatile("msr daifclr, #2" ::: "memory");   // unmask IRQ

    for (;;) __asm__ volatile("wfi");
}
