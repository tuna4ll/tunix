#ifndef TUNIX_VMM_H
#define TUNIX_VMM_H

#include <stddef.h>
#include <stdint.h>

#define KERNEL_BASE 0xFFFFFFFF80000000ULL
#define DIRECT_MAP_BASE 0xFFFFFE8000000000ULL
#define HEAP_VIRTUAL_BASE 0xFFFFFF0000000000ULL
#define DEVICE_MMIO_VIRTUAL_BASE 0xFFFFFFFFFF000000ULL
#define DEVICE_MMIO_VIRTUAL_BYTES 0x01000000ULL
#define DEVICE_MMIO_ARENA_OFFSET 0x00700000ULL
#define USER_ADDRESS_LIMIT 0x0000800000000000ULL

#define USER_STACK_TOP 0x00007FFFFFF00000ULL
#define USER_STACK_INITIAL_PAGES 32ULL
#define USER_STACK_MAX_PAGES 2048ULL
#define USER_STACK_LIMIT (USER_STACK_TOP - USER_STACK_MAX_PAGES * 4096ULL)

#define PAGE_PRESENT  (1ULL << 0)
#define PAGE_WRITE    (1ULL << 1)
#define PAGE_USER     (1ULL << 2)
#define PAGE_WRITE_THROUGH (1ULL << 3)
#define PAGE_UNCACHED (1ULL << 4)
#define PAGE_HUGE     (1ULL << 7)
#define PAGE_WRITE_COMBINING (1ULL << 7)
#define PAGE_DEVICE   (1ULL << 9)
#define PAGE_COW      (1ULL << 10)
#define PAGE_SHARED   (1ULL << 11)
#define PAGE_FILEBACKED (1ULL << 52)
#define PAGE_NX       (1ULL << 63)

void vmm_init(void);
int vmm_write_combining_available(void);

void vmm_configure_processor(void);

void *vmm_phys_to_virt(uint64_t physical);
uint64_t vmm_virt_to_phys_direct(const void *virtual_address);
uint64_t vmm_dma_physical(const void *pointer, uint64_t length);
uint64_t vmm_kernel_cr3(void);
uint64_t vmm_map_device(uint64_t physical, uint64_t bytes);
uint64_t vmm_current_cr3(void);
uint64_t vmm_create_address_space(void);
uint64_t vmm_clone_address_space(uint64_t source_cr3);
int vmm_handle_cow_fault(uint64_t cr3_physical, uint64_t virtual_address);
void vmm_destroy_address_space(uint64_t cr3_physical);
void vmm_activate(uint64_t cr3_physical);
int vmm_map_page_in(uint64_t cr3_physical, uint64_t virtual_address,
                    uint64_t physical_address, uint64_t flags);
int vmm_unmap_page_in(uint64_t cr3_physical, uint64_t virtual_address);
void vmm_prune_empty_tables(uint64_t cr3_physical, uint64_t start, uint64_t end);

void vmm_flush_batch_begin(void);
void vmm_flush_batch_end(void);
int vmm_protect_page_in(uint64_t cr3_physical, uint64_t virtual_address, uint64_t flags);
int vmm_translate(uint64_t cr3_physical, uint64_t virtual_address,
                  uint64_t *physical_out, uint64_t *flags_out);
int vmm_user_range_valid(uint64_t cr3_physical, uint64_t address,
                         size_t length, int write_required);
int vmm_copy_from_space(uint64_t cr3_physical, void *destination,
                        uint64_t source_user, size_t length);
int vmm_copy_to_space(uint64_t cr3_physical, uint64_t destination_user,
                      const void *source, size_t length);

void vmm_map_page(uint64_t virtual_address, uint64_t physical_address, uint16_t flags);
void vmm_unmap_page(uint64_t virtual_address);
uint64_t vmm_count_user_pages(uint64_t cr3_physical);

#endif
