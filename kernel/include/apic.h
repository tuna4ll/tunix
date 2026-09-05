#ifndef TUNIX_APIC_H
#define TUNIX_APIC_H

#include <stdint.h>

/* Enable the local APIC, mask the 8259 pair, and take over delivery. Returns
   -1 on a machine whose tables do not describe an IOAPIC, where the caller
   should stay on the PIC. */
int apic_init(void);

/* True once delivery has moved, which decides where an interrupt is
   acknowledged. */
int apic_is_active(void);
void apic_send_eoi(void);

/* Route one of the sixteen lines the PIC used to own, following whatever the
   firmware's overrides say about where it really arrives and how it is wired. */
int apic_route_legacy_irq(unsigned irq);

/* The same, for a line whose global number is already known -- a PCI device
   reading its own routing, once there is anything that does. */
int apic_route_irq(unsigned global, unsigned vector, int active_low,
                   int level_triggered);

/* Route an interrupt the firmware names by global number rather than by legacy
   line, which is how the FADT names the SCI. How it is wired comes from the
   MADT's overrides, and only from the ACPI default of level-triggered active
   low when nothing says otherwise -- the default is not a safe guess on its
   own, since QEMU wires its own SCI active high and says so in the table. */
int apic_route_global(unsigned global, unsigned vector);

/* The identity of whichever processor asks. */
uint32_t apic_local_id(void);
/* Every processor has to enable its own local APIC before it can be sent
   anything, including a timer interrupt of its own. */
void apic_enable_local(void);

/* Waking another processor: an INIT resets it, and a startup names the page it
   begins executing at, in real mode. */
void apic_send_init(uint32_t apic_id);
void apic_send_startup(uint32_t apic_id, uint8_t page);
/* To every processor except the one sending, without naming any of them. */
void apic_send_ipi_to_others(uint8_t vector);

/* The per-processor timer, measured against the TSC because nothing reports
   the rate it counts at. */
void apic_timer_start(uint32_t hz, uint8_t vector);
/* What that calibration measured on one processor, for the hardware report. */
void apic_timer_calibration(unsigned index, uint64_t *measured_hz,
                            uint32_t *initial_count);

#endif
