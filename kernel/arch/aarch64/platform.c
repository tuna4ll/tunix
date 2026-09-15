#include <stdint.h>

#include "../../include/pci.h"
#include "../../include/platform.h"
#include "../../include/serial.h"
#include "../../include/vmm.h"
#include "aarch64.h"

extern void kprintf(const char *fmt, ...);

#define PSCI_SYSTEM_OFF 0x84000008ULL
#define PSCI_SYSTEM_RESET 0x84000009ULL

void arch_early_init(void) {
    serial_init();
}

void arch_cpu_init(void) {
}

void arch_route_legacy_interrupts(void) {
}

void arch_route_timer(void) {
    gic_enable_interrupt(aarch64_platform.timer_interrupt);
}

void set_kernel_stack(uint64_t stack_top) {
    (void)stack_top;
}

void aarch64_pci_init(void) {
    if (!aarch64_platform.ecam_physical) return;
    uint32_t buses = aarch64_platform.ecam_last_bus - aarch64_platform.ecam_first_bus + 1U;
    uint64_t bytes = (uint64_t)buses << 20;
    if (bytes > aarch64_platform.ecam_size) bytes = aarch64_platform.ecam_size;
    if (bytes > AARCH64_ECAM_VIRTUAL_BYTES) bytes = AARCH64_ECAM_VIRTUAL_BYTES;
    for (uint64_t offset = 0; offset < bytes; offset += 4096ULL) {
        if (vmm_map_page_in(vmm_kernel_cr3(), AARCH64_ECAM_VIRTUAL_BASE + offset,
                            aarch64_platform.ecam_physical + offset,
                            PAGE_WRITE | PAGE_DEVICE | PAGE_UNCACHED | PAGE_NX) != 0) {
            kprintf("PCI: ECAM mapping failed at %p\n", (void *)offset);
            return;
        }
    }
    uint32_t last = aarch64_platform.ecam_first_bus + (uint32_t)(bytes >> 20) - 1U;
    pci_ecam_attach(AARCH64_ECAM_VIRTUAL_BASE, (uint8_t)aarch64_platform.ecam_first_bus,
                    (uint8_t)last);
    pci_assign_resources(aarch64_platform.pci_mmio32_base, aarch64_platform.pci_mmio32_size,
                         aarch64_platform.pci_mmio64_base, aarch64_platform.pci_mmio64_size);
    kprintf("PCI: ECAM at %p, buses %u-%u\n", (void *)aarch64_platform.ecam_physical,
            aarch64_platform.ecam_first_bus, last);
}

void arch_probe_buses(void) {
    gic_init();
    aarch64_pci_init();
}

extern uint64_t psci_smc(uint64_t function);
extern uint64_t psci_hvc(uint64_t function);

static void psci_call(uint64_t function) {
    if (aarch64_platform.psci_method == PSCI_SMC) psci_smc(function);
    else if (aarch64_platform.psci_method == PSCI_HVC) psci_hvc(function);
}

void psci_system_off(void) {
    psci_call(PSCI_SYSTEM_OFF);
}

void psci_system_reset(void) {
    psci_call(PSCI_SYSTEM_RESET);
}
