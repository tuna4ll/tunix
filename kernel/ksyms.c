#include <stdint.h>

#include "include/dma.h"
#include "include/heap.h"
#include "include/irq.h"
#include "include/kstring.h"
#include "include/module.h"
#if defined(__x86_64__)
#include "include/apic.h"
#endif
#include "include/pci.h"
#include "include/sound.h"
#include "include/pmm.h"
#include "include/net/net.h"
#include "include/time.h"
#include "include/vmm.h"

extern void kprintf(const char *fmt, ...);
extern void panic(const char *message);

#define EXPORT(symbol) { #symbol, (uint64_t)(uintptr_t)&symbol }

const struct module_export kernel_symbols[] = {
    EXPORT(kprintf),
    EXPORT(panic),
    EXPORT(kmalloc),
    EXPORT(kfree),
    EXPORT(memset),
    EXPORT(memcpy),
    EXPORT(memmove),
    EXPORT(strlen),
    EXPORT(strcmp),
    EXPORT(strncmp),
    EXPORT(strncpy),
    EXPORT(dma_alloc),
    EXPORT(dma_free),
    EXPORT(pmm_alloc_page),
    EXPORT(pmm_alloc_pages),
    EXPORT(pmm_free_page),
    EXPORT(pmm_free_pages),
    EXPORT(vmm_phys_to_virt),
    EXPORT(vmm_virt_to_phys_direct),
    EXPORT(vmm_dma_physical),
    EXPORT(vmm_map_device),
    EXPORT(vmm_map_page_in),
    EXPORT(vmm_kernel_cr3),
    EXPORT(pci_config_read32),
    EXPORT(pci_config_write32),
    EXPORT(pci_find_device),
    EXPORT(pci_find_class),
    EXPORT(pci_find_nth_class),
    EXPORT(pci_for_each_device),
    EXPORT(pci_enable_bus_mastering),
    EXPORT(pci_bar_address),
    EXPORT(pci_find_capability),
    EXPORT(pci_msix_enable),
    EXPORT(pci_msix_bind),
    EXPORT(pci_msi_bind),
    EXPORT(irq_request),
    EXPORT(irq_release),
    EXPORT(net_register_adapter),
    EXPORT(net_unregister_adapter),
    EXPORT(time_uptime_ns),
    EXPORT(time_realtime_ns),
    EXPORT(time_tsc_frequency),
    EXPORT(module_get),
    EXPORT(module_put),
    EXPORT(module_active),
    EXPORT(pci_register_driver),
    EXPORT(pci_unregister_driver),
    EXPORT(pci_device_driver),
    EXPORT(snd_register_card),
    EXPORT(snd_unregister_card),
#if defined(__x86_64__)
    EXPORT(apic_route_global),
#endif
};

const unsigned kernel_symbol_count =
    (unsigned)(sizeof(kernel_symbols) / sizeof(kernel_symbols[0]));
