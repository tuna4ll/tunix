#include <stddef.h>
#include <stdint.h>
#include "../include/io.h"
#include "../include/kstring.h"
#include "../include/pci.h"
#include "../include/vmm.h"

#define PCI_ADDRESS 0xCF8U
#define PCI_DATA 0xCFCU

#define PCI_COMMAND 0x04U
#define PCI_STATUS_HAS_CAPABILITIES (1U << 4)
#define PCI_CAPABILITY_POINTER 0x34U
#define PCI_CAP_ID_MSIX 0x11U

/* Bit 10 of the command register. With MSI-X in use the pin must be silenced:
   a device that signals both ways delivers every interrupt twice, once on a
   vector this kernel routed and once on a line it did not. */
#define PCI_COMMAND_INTX_DISABLE (1U << 10)

/* MSI-X capability, as offsets from the capability itself. */
#define MSIX_MESSAGE_CONTROL 2U
#define MSIX_TABLE_LOCATION 4U
#define MSIX_CONTROL_ENABLE (1U << 15)
#define MSIX_CONTROL_FUNCTION_MASK (1U << 14)
#define MSIX_CONTROL_TABLE_SIZE_MASK 0x07FFU
#define MSIX_TABLE_BIR_MASK 0x7U

/* One table entry: two words of address, the message, and a mask bit. */
#define MSIX_ENTRY_WORDS 4U
#define MSIX_ENTRY_ADDRESS_LOW 0U
#define MSIX_ENTRY_ADDRESS_HIGH 1U
#define MSIX_ENTRY_DATA 2U
#define MSIX_ENTRY_CONTROL 3U
#define MSIX_ENTRY_MASKED 1U

/*
 * Where a message goes on x86: the local APIC answers a write to this address
 * as an interrupt, with the destination processor in bits 19..12 and the
 * vector in the data word. It is not memory and there is nothing at that
 * physical address; the northbridge recognises the range and turns the write
 * into a delivery.
 */
#define APIC_MESSAGE_ADDRESS 0xFEE00000U
#define APIC_MESSAGE_DESTINATION_SHIFT 12U

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

    uint8_t capability = pci_find_capability(out, PCI_CAP_ID_MSIX);
    if (capability) {
        uint32_t header = pci_config_read32(bus, slot, function, capability);
        uint16_t control = (uint16_t)(header >> 16);
        out->msix_capability = capability;
        out->msix_entries = (uint16_t)((control & MSIX_CONTROL_TABLE_SIZE_MASK) + 1U);
    }
}

uint64_t pci_bar_address(const struct pci_device *device, unsigned index) {
    if (!device || index >= 6U) return 0;
    uint32_t low = device->bar[index];
    /* Bit 0 set means the BAR is in I/O space, where there is no address to
       map. Bits 2:1 say 10b for a 64-bit BAR, whose upper half is the next
       BAR along -- so a 64-bit BAR occupies two slots and the second one is
       not a BAR of its own. */
    if (!low || (low & 1U)) return 0;
    uint64_t address = low & 0xFFFFFFF0U;
    if (((low >> 1U) & 3U) == 2U) {
        if (index + 1U >= 6U) return 0;
        address |= (uint64_t)device->bar[index + 1U] << 32;
    }
    return address;
}

uint8_t pci_find_capability(const struct pci_device *device, uint8_t id) {
    if (!device) return 0;
    uint32_t status_command = pci_config_read32(device->bus, device->slot,
                                                device->function, PCI_COMMAND);
    if (!((status_command >> 16) & PCI_STATUS_HAS_CAPABILITIES)) return 0;

    uint32_t pointer = pci_config_read32(device->bus, device->slot, device->function,
                                         PCI_CAPABILITY_POINTER);
    uint8_t offset = (uint8_t)(pointer & 0xFCU);
    /* Bounded: a device whose capability list points at itself would otherwise
       be an infinite loop inside enumeration, before there is any way to say
       so. */
    for (unsigned guard = 0; offset && guard < 48U; guard++) {
        uint32_t header = pci_config_read32(device->bus, device->slot,
                                            device->function, offset);
        if ((uint8_t)header == id) return offset;
        offset = (uint8_t)((header >> 8) & 0xFCU);
    }
    return 0;
}

/* Bits 31..24 of EBX from leaf 1: the identifier the local APIC was given at
   reset, readable whether or not anything has mapped it. */
static uint32_t initial_apic_id(void) {
    uint32_t eax, ebx, ecx, edx;
    __asm__ volatile("cpuid"
                     : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                     : "a"(1U), "c"(0U));
    return ebx >> 24;
}

static void msix_write_control(const struct pci_device *device, uint16_t control) {
    uint32_t header = pci_config_read32(device->bus, device->slot, device->function,
                                        device->msix_capability);
    header = (header & 0x0000FFFFU) | ((uint32_t)control << 16);
    pci_config_write32(device->bus, device->slot, device->function,
                       device->msix_capability, header);
}

int pci_msix_enable(struct pci_device *device) {
    if (!device || !device->msix_capability || !device->msix_entries) return -1;
    if (device->msix_table) return 0;

    uint32_t location = pci_config_read32(device->bus, device->slot, device->function,
                                          (uint8_t)(device->msix_capability +
                                                    MSIX_TABLE_LOCATION));
    unsigned bar = location & MSIX_TABLE_BIR_MASK;
    uint64_t physical = pci_bar_address(device, bar);
    if (!physical) return -1;

    uint64_t table = vmm_map_device(physical + (location & ~MSIX_TABLE_BIR_MASK),
                                    (uint64_t)device->msix_entries * MSIX_ENTRY_WORDS *
                                        sizeof(uint32_t));
    if (!table) return -1;
    device->msix_table = (volatile uint32_t *)table;

    /* Every entry masked before the capability is enabled, because what the
       table holds until then is whatever was in that memory: an unmasked entry
       would point some vector at an address nobody chose. */
    for (unsigned entry = 0; entry < device->msix_entries; entry++)
        device->msix_table[entry * MSIX_ENTRY_WORDS + MSIX_ENTRY_CONTROL] =
            MSIX_ENTRY_MASKED;

    uint32_t command = pci_config_read32(device->bus, device->slot, device->function,
                                         PCI_COMMAND);
    pci_config_write32(device->bus, device->slot, device->function, PCI_COMMAND,
                       command | PCI_COMMAND_INTX_DISABLE);

    /* Function mask cleared in the same write that enables: leaving it set is
       a second, easily forgotten mask over the per-entry ones. */
    msix_write_control(device, MSIX_CONTROL_ENABLE);
    return 0;
}

int pci_msix_bind(struct pci_device *device, unsigned entry, unsigned vector) {
    if (!device || !device->msix_table) return -1;
    if (entry >= device->msix_entries) return -1;
    if (vector < 32U || vector > 255U) return -1;

    volatile uint32_t *slot = device->msix_table + entry * MSIX_ENTRY_WORDS;
    /* Whichever processor is doing the setting up, which is the one that
       booted: there is no interrupt balancing here, and a device pointed at a
       processor that never comes up interrupts nobody.
       Asked of the processor rather than of the local APIC, because a driver
       may bind a vector before apic_init() has mapped one -- virtio-gpu does,
       and the address it wrote was right only because the id it did not have
       happened to be zero. */
    slot[MSIX_ENTRY_ADDRESS_LOW] =
        APIC_MESSAGE_ADDRESS | (initial_apic_id() << APIC_MESSAGE_DESTINATION_SHIFT);
    slot[MSIX_ENTRY_ADDRESS_HIGH] = 0;
    slot[MSIX_ENTRY_DATA] = vector;
    /* Unmasked last, once the entry describes somewhere real. */
    slot[MSIX_ENTRY_CONTROL] = 0;
    return 0;
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

/*
 * The nth device of a class, counted in bus order.
 *
 * pci_find_class() answering with the first one is enough for a controller a
 * machine has one of. USB is not that: the same class and subclass cover UHCI,
 * OHCI, EHCI and xHCI, only the programming interface separates them, and a
 * machine of a certain age has several -- old Intel chipsets split their ports
 * across two EHCI controllers, and a machine with xHCI usually has an EHCI
 * beside it. Stopping at the first one found means missing whichever one the
 * disk is actually on.
 */
int pci_find_nth_class(uint8_t class_code, uint8_t subclass, unsigned nth,
                       struct pci_device *out) {
    if (!out) return -1;
    unsigned seen = 0;
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
                if ((uint8_t)(class_value >> 24) != class_code ||
                    (uint8_t)(class_value >> 16) != subclass) continue;
                if (seen++ != nth) continue;
                fill_device(out, (uint8_t)bus, (uint8_t)slot, (uint8_t)function);
                return 0;
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
