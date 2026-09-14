#ifndef TUNIX_AARCH64_ARCH_H
#define TUNIX_AARCH64_ARCH_H

#include <stddef.h>
#include <stdint.h>

#define UART0_BASE   0x09000000UL
#define GICD_BASE    0x08000000UL
#define GICR_BASE    0x080A0000UL

#define TIMER_PPI_INTID 30U

static inline void mmio_write32(uint64_t addr, uint32_t value) {
    *(volatile uint32_t *)addr = value;
}

static inline uint32_t mmio_read32(uint64_t addr) {
    return *(volatile uint32_t *)addr;
}

#define sysreg_write(reg, value)                                              \
    __asm__ volatile("msr " reg ", %0" : : "r"((uint64_t)(value)) : "memory")

#define sysreg_read(reg)                                                      \
    ({ uint64_t __v; __asm__ volatile("mrs %0, " reg : "=r"(__v)); __v; })

static inline void isb(void) { __asm__ volatile("isb" ::: "memory"); }
static inline void dsb_sy(void) { __asm__ volatile("dsb sy" ::: "memory"); }

void uart_init(void);
void uart_putc(char c);
void uart_puts(const char *s);
void kprintf(const char *fmt, ...);

void mmu_init(void);
void gic_init(void);
void gic_eoi(uint32_t intid);
uint32_t gic_acknowledge(void);
void timer_init(void);
void timer_tick(void);
uint64_t timer_ticks(void);

const void *fdt_find(const void *hint);
int fdt_probe(const void *dtb, uint64_t *ram_base, uint64_t *ram_bytes,
              uint32_t *cpu_count);

void pmm_init(uint64_t ram_base, uint64_t ram_bytes, uint64_t dtb, uint64_t dtb_size);
void *pmm_alloc_page(void);
void pmm_free_page(void *pa);
uint64_t pmm_free_pages(void);

int vmm_map_page(uint64_t va, uint64_t pa, int writable);
int vmm_unmap_page(uint64_t va);

#endif
