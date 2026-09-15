#ifndef TUNIX_ARCH_AARCH64_H
#define TUNIX_ARCH_AARCH64_H

#include <stddef.h>
#include <stdint.h>

#include "../../include/interrupt.h"
#include "../../include/syscall.h"

#define AARCH64_KERNEL_VIRTUAL_BASE 0xFFFFFFFF80000000ULL
#define AARCH64_EARLY_DEVICE_BASE 0xFFFFFFFFFE000000ULL
#define AARCH64_EARLY_DEVICE_PAGES 32U
#define AARCH64_ECAM_VIRTUAL_BASE 0xFFFFFFFFE0000000ULL
#define AARCH64_ECAM_VIRTUAL_BYTES 0x10000000ULL
#define AARCH64_RAM_RANGES 16U
#define AARCH64_MAX_CPUS 8U
#define AARCH64_SGI_FLUSH 1U

struct fdt_node {
    uint32_t offset;
    uint32_t address_cells;
    uint32_t size_cells;
};

int fdt_init(const void *blob);
uint32_t fdt_total_size(void);
int fdt_find_path(const char *path, struct fdt_node *out);
int fdt_find_compatible(const char *compatible, unsigned index, struct fdt_node *out);
int fdt_find_device_type(const char *type, unsigned index, struct fdt_node *out);
int fdt_child(const struct fdt_node *parent, unsigned index, struct fdt_node *out);
const void *fdt_property(const struct fdt_node *node, const char *name, uint32_t *length);
int fdt_is_compatible(const struct fdt_node *node, const char *compatible);
int fdt_reg(const struct fdt_node *node, unsigned index, uint64_t *base, uint64_t *size);
int fdt_reg_cpu(const struct fdt_node *node, unsigned index, uint64_t *base, uint64_t *size);
uint32_t fdt_read32(const void *cell);
int fdt_memreserve(unsigned index, uint64_t *base, uint64_t *size);

struct aarch64_range {
    uint64_t base;
    uint64_t size;
};

struct aarch64_cpu {
    uint64_t mpidr;
    uint64_t release_address;
};

struct aarch64_platform {
    struct aarch64_range ram[AARCH64_RAM_RANGES];
    unsigned ram_count;
    uint64_t load_offset;
    int gic_version;
    uint64_t gic_distributor;
    uint64_t gic_redistributor;
    uint64_t gic_redistributor_size;
    uint64_t gic_cpu_interface;
    uint32_t timer_interrupt;
    uint64_t timer_frequency;
    uint64_t rtc_base;
    uint64_t ecam_physical;
    uint64_t ecam_size;
    uint32_t ecam_first_bus;
    uint32_t ecam_last_bus;
    uint64_t pci_mmio32_base;
    uint64_t pci_mmio32_size;
    uint64_t pci_mmio64_base;
    uint64_t pci_mmio64_size;
    uint32_t cpu_count;
    struct aarch64_cpu cpus[AARCH64_MAX_CPUS];
    int psci_method;
};

#define PSCI_NONE 0
#define PSCI_HVC 1
#define PSCI_SMC 2

extern struct aarch64_platform aarch64_platform;
extern uint64_t boot_root[];

void aarch64_build_early_tables(uint64_t load_physical, uint64_t dtb_physical);
int aarch64_early_map(uint64_t virtual_address, uint64_t physical, uint64_t attributes,
                      int level);
uint64_t aarch64_early_map_device(uint64_t physical, uint64_t bytes);
int aarch64_physical_is_ram(uint64_t physical);

void aarch64_start(uint64_t dtb_physical, uint64_t load_physical);
int aarch64_el0_sync(struct syscall_frame *frame);
void aarch64_el1_sync(struct syscall_frame *frame);
int aarch64_irq(struct interrupt_frame *frame);
void aarch64_secondary_start(uint64_t index);
void aarch64_unexpected(struct syscall_frame *frame, uint64_t kind);
uint64_t aarch64_syscall_number(uint64_t generic);

void gic_init(void);
void gic_init_secondary(unsigned index);
void gic_send_flush_ipi(void);
void gic_enable_interrupt(uint32_t intid);
uint32_t gic_acknowledge(void);
void gic_end_of_interrupt(uint32_t intid);
void aarch64_timer_rearm(void);
void aarch64_pci_init(void);
uint64_t psci_call(uint64_t function, uint64_t first, uint64_t second, uint64_t third);
void psci_system_off(void);
void psci_system_reset(void);

#endif
