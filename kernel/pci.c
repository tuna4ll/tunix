#include <stdint.h>
#include "include/io.h"
#include "include/kstring.h"
#include "include/pci.h"

#define PCI_ADDRESS 0xCF8U
#define PCI_DATA 0xCFCU

static uint32_t pci_address(uint8_t bus, uint8_t slot, uint8_t function, uint8_t offset) {
    return 0x80000000U | ((uint32_t)bus << 16) | ((uint32_t)slot << 11) |
           ((uint32_t)function << 8) | (offset & 0xFCU);
}

uint32_t pci_config_read32(uint8_t bus, uint8_t slot, uint8_t function, uint8_t offset) {
    outl(PCI_ADDRESS, pci_address(bus, slot, function, offset));
    return inl(PCI_DATA);
}

void pci_config_write32(uint8_t bus, uint8_t slot, uint8_t function, uint8_t offset, uint32_t value) {
    outl(PCI_ADDRESS, pci_address(bus, slot, function, offset));
    outl(PCI_DATA, value);
}

static void fill_device(struct pci_device *out, uint8_t bus, uint8_t slot, uint8_t function) {
    memset(out, 0, sizeof(*out));
    out->bus = bus;
    out->slot = slot;
    out->function = function;
    uint32_t id = pci_config_read32(bus, slot, function, 0x00);
    out->vendor_id = (uint16_t)id;
    out->device_id = (uint16_t)(id >> 16);
    uint32_t class_value = pci_config_read32(bus, slot, function, 0x08);
    out->prog_if = (uint8_t)(class_value >> 8);
    out->subclass = (uint8_t)(class_value >> 16);
    out->class_code = (uint8_t)(class_value >> 24);
    for (unsigned index = 0; index < 6; index++)
        out->bar[index] = pci_config_read32(bus, slot, function, (uint8_t)(0x10 + index * 4));
    out->irq_line = (uint8_t)pci_config_read32(bus, slot, function, 0x3C);
}

int pci_find_device(uint16_t vendor_id, uint16_t device_id, struct pci_device *out) {
    if (!out) return -1;
    for (unsigned bus = 0; bus < 256; bus++) {
        for (unsigned slot = 0; slot < 32; slot++) {
            uint32_t id0 = pci_config_read32((uint8_t)bus, (uint8_t)slot, 0, 0);
            if ((uint16_t)id0 == 0xFFFFU) continue;
            uint32_t header = pci_config_read32((uint8_t)bus, (uint8_t)slot, 0, 0x0C);
            unsigned functions = (header & 0x00800000U) ? 8U : 1U;
            for (unsigned function = 0; function < functions; function++) {
                uint32_t id = pci_config_read32((uint8_t)bus, (uint8_t)slot, (uint8_t)function, 0);
                if ((uint16_t)id == vendor_id && (uint16_t)(id >> 16) == device_id) {
                    fill_device(out, (uint8_t)bus, (uint8_t)slot, (uint8_t)function);
                    return 0;
                }
            }
        }
    }
    return -1;
}

int pci_find_class(uint8_t class_code, uint8_t subclass, struct pci_device *out) {
    if (!out) return -1;
    for (unsigned bus = 0; bus < 256; bus++) {
        for (unsigned slot = 0; slot < 32; slot++) {
            uint32_t id0 = pci_config_read32((uint8_t)bus, (uint8_t)slot, 0, 0);
            if ((uint16_t)id0 == 0xFFFFU) continue;
            uint32_t header = pci_config_read32((uint8_t)bus, (uint8_t)slot, 0, 0x0C);
            unsigned functions = (header & 0x00800000U) ? 8U : 1U;
            for (unsigned function = 0; function < functions; function++) {
                uint32_t id = pci_config_read32((uint8_t)bus, (uint8_t)slot,
                                                (uint8_t)function, 0);
                if ((uint16_t)id == 0xFFFFU) continue;
                uint32_t class_value = pci_config_read32((uint8_t)bus, (uint8_t)slot,
                                                         (uint8_t)function, 0x08);
                if ((uint8_t)(class_value >> 24) == class_code &&
                    (uint8_t)(class_value >> 16) == subclass) {
                    fill_device(out, (uint8_t)bus, (uint8_t)slot,
                                (uint8_t)function);
                    return 0;
                }
            }
        }
    }
    return -1;
}

void pci_enable_bus_mastering(const struct pci_device *device) {
    if (!device) return;
    uint32_t value = pci_config_read32(device->bus, device->slot, device->function, 0x04);
    value |= 0x00000005U;
    pci_config_write32(device->bus, device->slot, device->function, 0x04, value);
}
