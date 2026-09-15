#include <stdint.h>

#include "../../include/acpi.h"
#include "../../include/apic.h"
#include "../../include/gdt.h"
#include "../../include/idt.h"
#include "../../include/input.h"
#include "../../include/pic.h"
#include "../../include/platform.h"
#include "../../include/serial.h"

void arch_early_init(void) {
    pic_init();
    serial_init();
}

void arch_cpu_init(void) {
    gdt_init();
    idt_init();
}

void arch_route_legacy_interrupts(void) {
    int apic = apic_init() == 0;
    if (apic) apic_route_legacy_irq(1U);
    else pic_unmask(1U);
    if (input_mouse_available()) {
        if (apic) apic_route_legacy_irq(12U); else pic_unmask(12U);
    }
    if (apic) acpi_power_button_enable(ACPI_SCI_VECTOR);
}

void arch_route_timer(void) {
    if (apic_is_active()) apic_route_legacy_irq(0U); else pic_unmask(0U);
}
