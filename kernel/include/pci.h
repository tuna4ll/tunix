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
    uint8_t msix_capability;
    uint16_t msix_entries;
    volatile uint32_t *msix_table;
};

uint32_t pci_config_read32(uint8_t bus, uint8_t slot, uint8_t function, uint8_t offset);
void pci_config_write32(uint8_t bus, uint8_t slot, uint8_t function, uint8_t offset, uint32_t value);
int pci_find_device(uint16_t vendor_id, uint16_t device_id, struct pci_device *out);
int pci_find_class(uint8_t class_code, uint8_t subclass, struct pci_device *out);
int pci_find_nth_class(uint8_t class_code, uint8_t subclass, unsigned nth,
                       struct pci_device *out);

void pci_for_each_device(void (*visit)(const struct pci_device *, void *),
                         void *context);

void pci_enable_bus_mastering(const struct pci_device *device);

uint64_t pci_bar_address(const struct pci_device *device, unsigned index);

uint8_t pci_find_capability(const struct pci_device *device, uint8_t id);

int pci_msix_enable(struct pci_device *device);
int pci_msix_bind(struct pci_device *device, unsigned entry, unsigned vector);

#endif
