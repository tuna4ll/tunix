#ifndef TUNIX_PCI_H
#define TUNIX_PCI_H

#include <stddef.h>
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

#define PCI_ANY_ID 0xFFFFU

struct pci_device_id {
    uint16_t vendor;
    uint16_t device;
    uint16_t class_code;
    uint16_t subclass;
};

struct pci_driver {
    const char *name;
    const struct pci_device_id *ids;
    unsigned id_count;
    int (*probe)(const struct pci_device *device);
    void (*remove)(const struct pci_device *device);
    struct pci_driver *next;
};

int pci_register_driver(struct pci_driver *driver);
void pci_unregister_driver(struct pci_driver *driver);
const char *pci_device_driver(const struct pci_device *device);

void pci_ecam_attach(uint64_t virtual_base, uint8_t first_bus, uint8_t last_bus);
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

/* The string udev hands modprobe for this device, uppercase as Linux writes it. */
void pci_modalias(const struct pci_device *device, char *out, size_t capacity);

uint8_t pci_find_capability(const struct pci_device *device, uint8_t id);

int pci_msix_enable(struct pci_device *device);
void pci_assign_resources(uint64_t mmio32_base, uint64_t mmio32_size,
                          uint64_t mmio64_base, uint64_t mmio64_size);
int pci_msix_bind(struct pci_device *device, unsigned entry, unsigned vector);
int pci_msi_bind(struct pci_device *device, unsigned vector);

#endif
