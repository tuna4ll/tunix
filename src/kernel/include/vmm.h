#ifndef TUNIX_VMM_H
#define TUNIX_VMM_H

#include <stddef.h>
#include <stdint.h>

#define KERNEL_BASE 0xFFFFFFFF80000000ULL
#define USER_ADDRESS_LIMIT 0x0000800000000000ULL

/*
 * User stack layout. Only USER_STACK_INITIAL_PAGES are mapped when the image
 * is loaded; the rest is mapped on demand by the page-fault handler, so a
 * process pays for the pages it actually touches instead of reserving the
 * worst case up front. USER_STACK_MAX_PAGES is the ceiling a fault is still
 * allowed to grow into.
 */
#define USER_STACK_TOP 0x00007FFFFFF00000ULL
#define USER_STACK_INITIAL_PAGES 32ULL   /* 128 KiB mapped eagerly */
#define USER_STACK_MAX_PAGES 2048ULL     /* 8 MiB ceiling, as on Linux */
#define USER_STACK_LIMIT (USER_STACK_TOP - USER_STACK_MAX_PAGES * 4096ULL)

#define PAGE_PRESENT  (1ULL << 0)
#define PAGE_WRITE    (1ULL << 1)
#define PAGE_USER     (1ULL << 2)
#define PAGE_HUGE     (1ULL << 7)
#define PAGE_DEVICE   (1ULL << 9)
/* Software bit (the CPU ignores 9..11). Marks a page that fork shared instead
   of copying: it is mapped read-only in every owner, and the first write takes
   a fault that vmm_handle_cow_fault() turns into a private copy. */
#define PAGE_COW      (1ULL << 10)
/* Software bit. Marks a page belonging to a MAP_SHARED mapping, where the whole
   point is that writes *are* visible to the other owners. fork() must hand this
   one straight through -- copying it, whether eagerly or on write, would
   silently break the sharing. */
#define PAGE_SHARED   (1ULL << 11)
#define PAGE_NX       (1ULL << 63)

void vmm_init(void);
void *vmm_phys_to_virt(uint64_t physical);
uint64_t vmm_virt_to_phys_direct(const void *virtual_address);
uint64_t vmm_kernel_cr3(void);
uint64_t vmm_current_cr3(void);
uint64_t vmm_create_address_space(void);
uint64_t vmm_clone_address_space(uint64_t source_cr3);
/* Give a copy-on-write page back its write permission, copying it first if it
   still has other owners. 0 when the fault is handled, -1 when it was not a
   copy-on-write fault. */
int vmm_handle_cow_fault(uint64_t cr3_physical, uint64_t virtual_address);
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
