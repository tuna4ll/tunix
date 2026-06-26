#ifndef TUNIX_VMM_H
#define TUNIX_VMM_H

#include <stddef.h>
#include <stdint.h>

#define KERNEL_BASE 0xFFFFFFFF80000000ULL
#define USER_ADDRESS_LIMIT 0x0000800000000000ULL

#define PAGE_PRESENT  (1ULL << 0)
#define PAGE_WRITE    (1ULL << 1)
#define PAGE_USER     (1ULL << 2)
#define PAGE_HUGE     (1ULL << 7)
#define PAGE_DEVICE   (1ULL << 9)
#define PAGE_NX       (1ULL << 63)

void vmm_init(void);
void *vmm_phys_to_virt(uint64_t physical);
uint64_t vmm_virt_to_phys_direct(const void *virtual_address);
uint64_t vmm_kernel_cr3(void);
uint64_t vmm_current_cr3(void);
uint64_t vmm_create_address_space(void);
uint64_t vmm_clone_address_space(uint64_t source_cr3);
void vmm_destroy_address_space(uint64_t cr3_physical);
void vmm_activate(uint64_t cr3_physical);
int vmm_map_page_in(uint64_t cr3_physical, uint64_t virtual_address,
                    uint64_t physical_address, uint64_t flags);
int vmm_unmap_page_in(uint64_t cr3_physical, uint64_t virtual_address);
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
