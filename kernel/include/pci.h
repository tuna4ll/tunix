#ifndef TUNIX_PCI_H
#define TUNIX_PCI_H

#include <stdint.h>

struct pci_device {
    uint8_t bus;
    uint8_t slot;
    uint8_t function;
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t class_code;
    uint8_t subclass;
    uint8_t prog_if;
    uint8_t irq_line;
    uint32_t bar[6];
    /* Where the MSI-X capability sits in config space, and how many messages
       it has room for. Zero when the device has none, which is every device
       older than the idea and a few that are not. Filled in by enumeration
       because it costs one config read and answers "can this device interrupt
       us properly" without opening anything. */
    uint8_t msix_capability;
    uint16_t msix_entries;
    /* The table itself, once pci_msix_enable() has mapped it. */
    volatile uint32_t *msix_table;
};

uint32_t pci_config_read32(uint8_t bus, uint8_t slot, uint8_t function, uint8_t offset);
void pci_config_write32(uint8_t bus, uint8_t slot, uint8_t function, uint8_t offset, uint32_t value);
int pci_find_device(uint16_t vendor_id, uint16_t device_id, struct pci_device *out);
int pci_find_class(uint8_t class_code, uint8_t subclass, struct pci_device *out);
/* The nth device of a class, counted in bus order. What it is for is the four
   kinds of USB host controller, which share a class and a subclass and which a
   machine can have several of; see pci.c. */
int pci_find_nth_class(uint8_t class_code, uint8_t subclass, unsigned nth,
                       struct pci_device *out);

void pci_enable_bus_mastering(const struct pci_device *device);

/* Where a BAR points, with the two halves of a 64-bit one put together. 0 when
   the BAR is unset, or is in I/O space, or is the upper half of the one below
   it. */
uint64_t pci_bar_address(const struct pci_device *device, unsigned index);

/* The config offset of a capability, or 0. */
uint8_t pci_find_capability(const struct pci_device *device, uint8_t id);

/*
 * Message-signalled interrupts.
 *
 * A device with MSI-X does not have an interrupt line at all: it interrupts by
 * writing a value the driver chose to an address the driver chose, which on
 * x86 is the local APIC's doorbell. That is why this is worth having over the
 * IOAPIC path even when both work -- there is no line to share, no level to
 * acknowledge, and a device can raise as many distinct interrupts as it has
 * table entries.
 *
 * enable() maps the table, masks every entry and turns the capability on, so
 * nothing arrives until a bind() says where it should go. Each bind() points
 * one entry at one vector on this processor and unmasks it.
 */
int pci_msix_enable(struct pci_device *device);
int pci_msix_bind(struct pci_device *device, unsigned entry, unsigned vector);

#endif
