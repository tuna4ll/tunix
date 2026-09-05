#ifndef TUNIX_VMM_H
#define TUNIX_VMM_H

#include <stddef.h>
#include <stdint.h>

/*
 * Where the kernel image is linked: -mcmodel=kernel puts every symbol in the
 * top 2 GiB, so the code and its static data have no choice, and Limine
 * requires the same. Only the image is mapped here -- how far a static
 * buffer's address is from its physical one is the loader's to say, which is
 * why vmm_virt_to_phys_direct() asks boot_info() rather than subtracting this.
 */
#define KERNEL_BASE 0xFFFFFFFF80000000ULL
/*
 * The direct map: all of physical memory, in its own PML4 entry.
 *
 * It used to start at KERNEL_BASE and run up to the framebuffer window, which
 * put a hard 1792 MiB ceiling on how much RAM the machine could use -- not a
 * tuning choice but the distance between two fixed addresses in the top 2 GiB.
 * A browser tab is most of a gigabyte, so that ceiling was reachable by
 * ordinary use. Moving the map into a slot of its own, as the heap did before
 * it, replaces 1792 MiB of room with 512 GiB.
 */
#define DIRECT_MAP_BASE 0xFFFFFE8000000000ULL
/* The kernel heap, in a PML4 entry of its own. It used to sit directly above
   the direct map, which is what held the direct map -- and so the amount of
   RAM the machine could use -- to a single gigabyte. */
#define HEAP_VIRTUAL_BASE 0xFFFFFF0000000000ULL
/* Where device registers get mapped, at the very top of the address space.
   It shares the top 2 GiB with the kernel image and the framebuffer window,
   which is now all that is up here: the direct map moved out from under them.
   A huge page is not something a 4 KiB mapping can be placed inside, so these
   windows stay clear of the first gigabyte the image is mapped through. */
#define DEVICE_MMIO_VIRTUAL_BASE 0xFFFFFFFFFF000000ULL
#define DEVICE_MMIO_VIRTUAL_BYTES 0x01000000ULL
/*
 * Who owns what inside that window. Offsets are hard-coded by each driver, so
 * this list is the only thing keeping two of them apart -- and a collision does
 * not look like one: the second mapping just fails and the device looks absent.
 *
 *   0x000000  xHCI registers      (64 KiB)   src/kernel/drivers/usb/xhci.c
 *   0x100000  ACPI tables         (2 MiB)    src/kernel/drivers/acpi.c
 *   0x400000  local APIC, IOAPIC  (8 KiB)    src/kernel/drivers/apic.c
 *   0x500000  HD Audio registers  (16 KiB)   src/kernel/drivers/audio/hda.c
 *   0x600000  unused legacy virtio window
 *   0x700000  handed out by vmm_map_device(), including virtio BARs
 *             instead of picking an offset and hoping.
 */
#define DEVICE_MMIO_ARENA_OFFSET 0x00700000ULL
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
/* Registers are not memory: a cached mapping would let the processor answer a
   read from a line it fetched earlier, and hide a write in a store buffer,
   which is how a controller ends up looking dead when it is only unheard. */
#define PAGE_WRITE_THROUGH (1ULL << 3)
#define PAGE_UNCACHED (1ULL << 4)
#define PAGE_HUGE     (1ULL << 7)
/*
 * Write-combining, for the framebuffer.
 *
 * Bit 7 is the PAT selector in a *page table* entry and the huge-page bit in a
 * directory entry -- the same bit means two different things by level, which
 * is why this shares a value with PAGE_HUGE and must only ever be passed for a
 * 4 KiB mapping. With PWT and PCD clear it selects PAT entry 4, which
 * vmm_init() reprograms to write-combining.
 *
 * On real hardware this is not a tuning knob. A framebuffer mapped write-back
 * is wrong (the card never sees half the writes until something flushes) and
 * mapped uncached is correct but roughly two orders of magnitude slower, one
 * bus transaction per pixel. Write-combining is what makes a software-rendered
 * desktop possible at all. In QEMU the framebuffer is ordinary RAM and none of
 * this is observable, which is exactly why it is easy to get wrong and never
 * notice.
 */
#define PAGE_WRITE_COMBINING (1ULL << 7)
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
/* Software bit (the CPU ignores 52..58). Marks a copy-on-write page whose frame
   is the kernel's own cached copy of a file. The sole-owner shortcut in
   vmm_handle_cow_fault() must never fire for one: the refcount stops counting
   the cache once siblings copy away, and handing the frame back writable would
   let a process edit the kernel's copy of the file. */
#define PAGE_FILEBACKED (1ULL << 52)
#define PAGE_NX       (1ULL << 63)

void vmm_init(void);
/* True once PAT entry 4 has been set to write-combining, which is what makes
   PAGE_WRITE_COMBINING mean anything. False on a processor without PAT, where
   the caller should map the framebuffer uncached and accept the cost. */
int vmm_write_combining_available(void);

void *vmm_phys_to_virt(uint64_t physical);
uint64_t vmm_virt_to_phys_direct(const void *virtual_address);
/*
 * The physical address of a buffer a device may be pointed at, or 0.
 *
 * DMA needs the whole run to be physically contiguous, which is true of the
 * direct map and of the kernel image and of nothing else the kernel hands
 * around -- a heap allocation is pieced together from whatever pages the
 * allocator had. The drivers used to answer this by subtracting a constant,
 * which stopped being right the moment a loader was free to place the image
 * where it liked.
 */
uint64_t vmm_dma_physical(const void *pointer, uint64_t length);
uint64_t vmm_kernel_cr3(void);
/*
 * Map a device's registers and return the address they can be reached at, or 0.
 * The window above DEVICE_MMIO_ARENA_OFFSET is handed out here rather than
 * carved up by hand, because a driver that picks a colliding offset does not
 * fail loudly -- the second mapping is refused and the device simply looks
 * absent. `physical` need not be page aligned; the offset within the page is
 * preserved in the returned address.
 */
uint64_t vmm_map_device(uint64_t physical, uint64_t bytes);
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
/* Give back the tables an unmapped range left behind. */
void vmm_prune_empty_tables(uint64_t cr3_physical, uint64_t start, uint64_t end);
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
