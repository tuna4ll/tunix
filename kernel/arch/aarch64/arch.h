#ifndef TUNIX_AARCH64_ARCH_H
#define TUNIX_AARCH64_ARCH_H

#include <stddef.h>
#include <stdint.h>

#define KERNEL_VA_OFFSET 0xFFFF000000000000UL
#define KERNEL_PHYS_BASE 0x40000000UL

#define UART0_PHYS   0x09000000UL
#define GICD_PHYS    0x08000000UL
#define GICR_PHYS    0x080A0000UL

#define TIMER_PPI_INTID 30U

extern uint64_t va_offset;              // 0 until the kernel runs virtual

static inline uint64_t phys_to_virt(uint64_t pa) { return pa + va_offset; }
static inline uint64_t virt_to_phys(uint64_t va) { return va - va_offset; }

#define UART0_BASE phys_to_virt(UART0_PHYS)
#define GICD_BASE  phys_to_virt(GICD_PHYS)
#define GICR_BASE  phys_to_virt(GICR_PHYS)

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

// Must match SAVE_FRAME/RESTORE_FRAME in exceptions.S.
struct trap_frame {
    uint64_t x[31];
    uint64_t elr;
    uint64_t spsr;
    uint64_t sp_el0;
    uint64_t reserved[2];
};
_Static_assert(sizeof(struct trap_frame) == 288, "trap frame layout");

void uart_init(void);
void uart_putc(char c);
void uart_puts(const char *s);
void kprintf(const char *fmt, ...);

void mmu_init(void);
void arch_va_online(void);
uint64_t *mmu_root_table(void);

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
void pmm_reserve(uint64_t start, uint64_t bytes);
void *pmm_alloc_page(void);
void *pmm_alloc_pages(unsigned count);
void pmm_free_page(void *pa);
uint64_t pmm_free_pages(void);

#define VMM_WRITE 1U
#define VMM_USER  2U
#define VMM_EXEC  4U

int vmm_map(uint64_t root_pa, uint64_t va, uint64_t pa, unsigned flags);
int vmm_unmap(uint64_t root_pa, uint64_t va);
int vmm_map_page(uint64_t va, uint64_t pa, int writable);
int vmm_unmap_page(uint64_t va);
uint64_t vmm_create_space(void);
void vmm_destroy_space(uint64_t root_pa);
void vmm_switch_space(uint64_t root_pa);

void heap_init(void);
void *kmalloc(size_t want);
void kfree(void *ptr);
uint64_t heap_free_bytes(void);

void aarch64_enter_user(uint64_t entry, uint64_t user_sp);
void aarch64_leave_user(void);
void aarch64_syscall_handler(struct trap_frame *frame);

struct elf_image {
    uint64_t entry;
    uint64_t phdr;                      // user address of the program headers
    uint16_t phentsize;
    uint16_t phnum;
};

int elf_load_image(uint64_t root_pa, const void *data, uint64_t length,
                   struct elf_image *out);

int user_stack_build(uint64_t stack_top, const char *const argv[],
                     const char *const envp[], const struct elf_image *image,
                     uint64_t *sp_out);

// QEMU virt lays 32 virtio-mmio slots out back to back, SPI 16 upwards.
#define VIRTIO_MMIO_BASE     0x0A000000UL
#define VIRTIO_MMIO_STRIDE   0x200UL
#define VIRTIO_MMIO_SLOTS    32U
#define VIRTIO_MMIO_IRQ_BASE 48U

uint64_t virtio_mmio_slot(unsigned slot);
int virtio_mmio_find(uint32_t device_id);
unsigned virtio_mmio_probe(void);
int virtio_blk_init(void);
uint64_t virtio_blk_capacity(void);
int virtio_blk_read(uint64_t sector, void *out);

int ext2_mount(void);
int ext2_lookup_path(const char *path, uint32_t *ino_out, uint32_t *size_out);
int ext2_read_file(uint32_t ino, void *out, uint32_t limit, uint32_t *size_out);

extern uint64_t *current_resume;
void aarch64_context_switch(uint64_t *save_sp, uint64_t new_sp);
void sched_init(void);
int sched_create(const char *name, void (*entry)(void *), void *argument);
void sched_yield(void);
void sched_exit(void);
void sched_tick(void);
void sched_set_space(uint64_t root_pa);
int sched_current_id(void);

#endif
