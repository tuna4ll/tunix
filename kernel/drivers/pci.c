#include <stddef.h>
#include <stdint.h>
#if defined(__x86_64__)
#include "../include/io.h"
#endif
#if defined(__aarch64__)
#include "../arch/aarch64/aarch64.h"
#include "../include/irq.h"
#endif
#include "../include/kstring.h"
#include "../include/pci.h"
#include "../include/vmm.h"

#define PCI_COMMAND 0x04U
#define PCI_STATUS_HAS_CAPABILITIES (1U << 4)
#define PCI_CAPABILITY_POINTER 0x34U
#define PCI_CAP_ID_MSIX 0x11U

#define PCI_COMMAND_INTX_DISABLE (1U << 10)

#define MSIX_MESSAGE_CONTROL 2U
#define MSIX_TABLE_LOCATION 4U
#define MSIX_CONTROL_ENABLE (1U << 15)
#define MSIX_CONTROL_FUNCTION_MASK (1U << 14)
#define MSIX_CONTROL_TABLE_SIZE_MASK 0x07FFU
#define MSIX_TABLE_BIR_MASK 0x7U

#define MSIX_ENTRY_WORDS 4U
#define MSIX_ENTRY_ADDRESS_LOW 0U
#define MSIX_ENTRY_ADDRESS_HIGH 1U
#define MSIX_ENTRY_DATA 2U
#define MSIX_ENTRY_CONTROL 3U
#define MSIX_ENTRY_MASKED 1U

#if defined(__x86_64__)

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

#else

static uint64_t ecam_base;
static uint8_t ecam_first_bus;
static uint8_t ecam_last_bus;

void pci_ecam_attach(uint64_t virtual_base, uint8_t first_bus, uint8_t last_bus) {
    ecam_first_bus = first_bus;
    ecam_last_bus = last_bus;
    ecam_base = virtual_base;
}

static volatile uint32_t *ecam_register(uint8_t bus, uint8_t slot, uint8_t function,
                                        uint8_t offset) {
    if (!ecam_base || bus < ecam_first_bus || bus > ecam_last_bus) return (volatile uint32_t *)0;
    if (slot >= 32U || function >= 8U) return (volatile uint32_t *)0;
    uint64_t index = ((uint64_t)(bus - ecam_first_bus) << 20) | ((uint64_t)slot << 15) |
                     ((uint64_t)function << 12) | (offset & 0xFCU);
    return (volatile uint32_t *)(ecam_base + index);
}

uint32_t pci_config_read32(uint8_t bus, uint8_t slot, uint8_t function, uint8_t offset) {
    volatile uint32_t *reg = ecam_register(bus, slot, function, offset);
    return reg ? *reg : 0xFFFFFFFFU;
}

struct bar_window {
    uint64_t next;
    uint64_t end;
};

static int window_take(struct bar_window *window, uint64_t size, uint64_t *address) {
    uint64_t base = (window->next + size - 1U) & ~(size - 1U);
    if (!window->end || base < window->next || base + size > window->end) return -1;
    *address = base;
    window->next = base + size;
    return 0;
}

static void assign_function(uint8_t bus, uint8_t slot, uint8_t function,
                            struct bar_window *low, struct bar_window *high) {
    uint32_t header = pci_config_read32(bus, slot, function, 0x0C);
    if (((header >> 16) & 0x7FU) != 0) return;
    uint32_t command = pci_config_read32(bus, slot, function, 0x04);
    pci_config_write32(bus, slot, function, 0x04, command & ~0x7U);

    for (unsigned index = 0; index < 6U; index++) {
        uint8_t offset = (uint8_t)(0x10U + index * 4U);
        uint32_t original = pci_config_read32(bus, slot, function, offset);
        if (original & 1U) continue;
        int wide = ((original >> 1) & 3U) == 2U;
        pci_config_write32(bus, slot, function, offset, 0xFFFFFFFFU);
        uint64_t mask = pci_config_read32(bus, slot, function, offset) & ~0xFULL;
        if (wide && index < 5U) {
            pci_config_write32(bus, slot, function, (uint8_t)(offset + 4U), 0xFFFFFFFFU);
            mask |= (uint64_t)pci_config_read32(bus, slot, function, (uint8_t)(offset + 4U)) << 32;
        } else {
            mask |= 0xFFFFFFFF00000000ULL;
        }
        if (!(mask & 0xFFFFFFF0ULL) && !(mask >> 32)) {
            pci_config_write32(bus, slot, function, offset, original);
            continue;
        }
        uint64_t size = ~mask + 1U;
        uint64_t address = 0;
        int placed = wide ? window_take(high, size, &address) : -1;
        if (placed != 0) placed = window_take(low, size, &address);
        if (placed != 0) {
            pci_config_write32(bus, slot, function, offset, original);
            if (wide) index++;
            continue;
        }
        pci_config_write32(bus, slot, function, offset, (uint32_t)address | (original & 0xFU));
        if (wide) {
            pci_config_write32(bus, slot, function, (uint8_t)(offset + 4U), (uint32_t)(address >> 32));
            index++;
        }
    }
    pci_config_write32(bus, slot, function, 0x04, command | 0x2U);
}

void pci_assign_resources(uint64_t mmio32_base, uint64_t mmio32_size,
                          uint64_t mmio64_base, uint64_t mmio64_size) {
    struct bar_window low = { mmio32_base, mmio32_size ? mmio32_base + mmio32_size : 0 };
    struct bar_window high = { mmio64_base, mmio64_size ? mmio64_base + mmio64_size : 0 };
    uint8_t bus = ecam_first_bus;
    for (unsigned slot = 0; slot < 32U; slot++) {
        uint32_t id = pci_config_read32(bus, (uint8_t)slot, 0, 0);
        if ((uint16_t)id == 0xFFFFU) continue;
        uint32_t header = pci_config_read32(bus, (uint8_t)slot, 0, 0x0C);
        unsigned functions = (header & 0x00800000U) ? 8U : 1U;
        for (unsigned function = 0; function < functions; function++) {
            if ((uint16_t)pci_config_read32(bus, (uint8_t)slot, (uint8_t)function, 0) == 0xFFFFU)
                continue;
            assign_function(bus, (uint8_t)slot, (uint8_t)function, &low, &high);
        }
    }
}

void pci_config_write32(uint8_t bus, uint8_t slot, uint8_t function, uint8_t offset, uint32_t value) {
    volatile uint32_t *reg = ecam_register(bus, slot, function, offset);
    if (reg) *reg = value;
}

#endif

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
    for (unsigned guard = 0; offset && guard < 48U; guard++) {
        uint32_t header = pci_config_read32(device->bus, device->slot,
                                            device->function, offset);
        if ((uint8_t)header == id) return offset;
        offset = (uint8_t)((header >> 8) & 0xFCU);
    }
    return 0;
}

#if defined(__x86_64__)
#define APIC_MESSAGE_ADDRESS 0xFEE00000U
#define APIC_MESSAGE_DESTINATION_SHIFT 12U

static uint32_t initial_apic_id(void) {
    uint32_t eax, ebx, ecx, edx;
    __asm__ volatile("cpuid"
                     : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                     : "a"(1U), "c"(0U));
    return ebx >> 24;
}

#endif

#if defined(__x86_64__) || defined(__aarch64__)
static void msix_write_control(const struct pci_device *device, uint16_t control) {
    uint32_t header = pci_config_read32(device->bus, device->slot, device->function,
                                        device->msix_capability);
    header = (header & 0x0000FFFFU) | ((uint32_t)control << 16);
    pci_config_write32(device->bus, device->slot, device->function,
                       device->msix_capability, header);
}
#endif

int pci_msix_enable(struct pci_device *device) {
#if !defined(__x86_64__) && !defined(__aarch64__)
    (void)device;
    return -1;
#else
    if (!device || !device->msix_capability || !device->msix_entries) return -1;
    if (device->msix_table) return 0;
#if defined(__aarch64__)
    if (!its_ready()) return -1;
#endif

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

    for (unsigned entry = 0; entry < device->msix_entries; entry++)
        device->msix_table[entry * MSIX_ENTRY_WORDS + MSIX_ENTRY_CONTROL] =
            MSIX_ENTRY_MASKED;

    uint32_t command = pci_config_read32(device->bus, device->slot, device->function,
                                         PCI_COMMAND);
    pci_config_write32(device->bus, device->slot, device->function, PCI_COMMAND,
                       command | PCI_COMMAND_INTX_DISABLE);

    msix_write_control(device, MSIX_CONTROL_ENABLE);
    return 0;
#endif
}

int pci_msix_bind(struct pci_device *device, unsigned entry, unsigned vector) {
    if (!device || !device->msix_table) return -1;
    if (entry >= device->msix_entries) return -1;
    if (vector < 32U || vector > 255U) return -1;

#if defined(__x86_64__)
    volatile uint32_t *slot = device->msix_table + entry * MSIX_ENTRY_WORDS;
    slot[MSIX_ENTRY_ADDRESS_LOW] =
        APIC_MESSAGE_ADDRESS | (initial_apic_id() << APIC_MESSAGE_DESTINATION_SHIFT);
    slot[MSIX_ENTRY_ADDRESS_HIGH] = 0;
    slot[MSIX_ENTRY_DATA] = vector;
    slot[MSIX_ENTRY_CONTROL] = 0;
    return 0;
#elif defined(__aarch64__)
    if (vector < IRQ_VECTOR_FIRST) return -1;
    uint32_t requester = ((uint32_t)device->bus << 8) | ((uint32_t)device->slot << 3) |
                         device->function;
    uint32_t event = vector - IRQ_VECTOR_FIRST;
    uint64_t address;
    if (its_bind_msi(requester - aarch64_platform.msi_rid_base +
                         aarch64_platform.msi_device_base,
                     event, &address) != 0)
        return -1;
    volatile uint32_t *message = device->msix_table + entry * MSIX_ENTRY_WORDS;
    message[MSIX_ENTRY_ADDRESS_LOW] = (uint32_t)address;
    message[MSIX_ENTRY_ADDRESS_HIGH] = (uint32_t)(address >> 32);
    message[MSIX_ENTRY_DATA] = event;
    message[MSIX_ENTRY_CONTROL] = 0;
    return 0;
#else
    return -1;
#endif
}

void pci_for_each_device(void (*visit)(const struct pci_device *, void *),
                         void *context) {
    if (!visit) return;
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
                struct pci_device device;
                fill_device(&device, (uint8_t)bus, (uint8_t)slot, (uint8_t)function);
                visit(&device, context);
            }
        }
    }
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
    value |= 0x00000007U;
    pci_config_write32(device->bus, device->slot, device->function, 0x04, value);
}
