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

#endif
