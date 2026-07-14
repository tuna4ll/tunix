#include "include/build_config.h"
#include <stddef.h>
#include <stdint.h>
#include "include/kstring.h"
#include "include/pmm.h"
#include "include/vmm.h"

#define ADDRESS_MASK 0x000FFFFFFFFFF000ULL
#define DIRECT_MAP_SIZE PMM_DIRECT_MAP_LIMIT
#define MAX_ADDRESS_SPACES 256

extern void panic(const char *msg);
extern void kprintf(const char *fmt, ...);

#if TUNIX_DEBUG_LOGS
#define KDEBUG(...) kprintf(__VA_ARGS__)
#else
#define KDEBUG(...) do { } while (0)
#endif

static uint64_t kernel_cr3_physical;
static uint64_t address_spaces[MAX_ADDRESS_SPACES];

static inline uint64_t read_cr3(void) {
    uint64_t value;
    __asm__ volatile("mov %%cr3, %0" : "=r"(value));
    return value & ADDRESS_MASK;
}

static inline void write_cr3(uint64_t value) {
    __asm__ volatile("mov %0, %%cr3" : : "r"(value) : "memory");
}

static inline void invalidate(uint64_t address) {
    __asm__ volatile("invlpg (%0)" : : "r"(address) : "memory");
}

static int physical_direct_range_valid(uint64_t physical, size_t length) {
    if (!length) return physical < DIRECT_MAP_SIZE;
    if (physical >= DIRECT_MAP_SIZE || length > DIRECT_MAP_SIZE - physical) return 0;
    return pmm_physical_range_managed(physical, (uint64_t)length);
}

static void report_bad_physical(const char *operation, uint64_t physical,
                                const void *caller) {
    kprintf("VMM: %s physical=%p caller=%p managed_limit=%p direct_limit=%p\n",
            operation, (void *)physical, (void *)caller,
            (void *)pmm_managed_limit(), (void *)DIRECT_MAP_SIZE);
}

void *vmm_phys_to_virt(uint64_t physical) {
    if (!physical_direct_range_valid(physical, 1)) {
        report_bad_physical("phys_to_virt rejected", physical, __builtin_return_address(0));
        panic("VMM: invalid physical address");
    }
    return (void *)(KERNEL_BASE + physical);
}

uint64_t vmm_virt_to_phys_direct(const void *virtual_address) {
    uint64_t value = (uint64_t)virtual_address;
    if (value < KERNEL_BASE || value >= KERNEL_BASE + DIRECT_MAP_SIZE) {
        panic("VMM: address is not in direct map");
    }
    uint64_t physical = value - KERNEL_BASE;
    if (!pmm_physical_range_managed(physical, 1)) {
        report_bad_physical("virt_to_phys rejected", physical, __builtin_return_address(0));
        panic("VMM: direct-map pointer outside managed RAM");
    }
    return physical;
}

static int registry_contains(const uint64_t *registry, size_t count, uint64_t value) {
    for (size_t index = 0; index < count; index++) {
        if (registry[index] == value) return 1;
    }
    return 0;
}

static int registry_add(uint64_t *registry, size_t count, uint64_t value) {
    if (registry_contains(registry, count, value)) return 0;
    for (size_t index = 0; index < count; index++) {
        if (registry[index] == 0) {
            registry[index] = value;
            return 0;
        }
    }
    return -1;
}

static void registry_remove(uint64_t *registry, size_t count, uint64_t value) {
    for (size_t index = 0; index < count; index++) {
        if (registry[index] == value) {
            registry[index] = 0;
            return;
        }
    }
}

static int address_space_registered(uint64_t cr3_physical) {
    uint64_t physical = cr3_physical & ADDRESS_MASK;
    return physical != 0 &&
           registry_contains(address_spaces, MAX_ADDRESS_SPACES, physical);
}

static uint64_t *page_table_pointer(uint64_t physical) {
    physical &= ADDRESS_MASK;
    if (!physical || !physical_direct_range_valid(physical, 4096) ||
        !pmm_page_is_allocated(physical)) {
        KDEBUG("VMM: rejected invalid page-table page %p\n", (void *)physical);
        return NULL;
    }
    return (uint64_t *)(KERNEL_BASE + physical);
}

static uint64_t *table_from_entry(uint64_t entry) {
    if (!(entry & PAGE_PRESENT) || (entry & PAGE_HUGE)) return NULL;
    return page_table_pointer(entry & ADDRESS_MASK);
}

static uint64_t *next_table(uint64_t *table, uint16_t index,
                            uint64_t leaf_flags, int create) {
    if (!table) return NULL;
    uint64_t entry = table[index];
    if (entry & PAGE_PRESENT) {
        if (entry & PAGE_HUGE) return NULL;
        if ((leaf_flags & PAGE_USER) && !(entry & PAGE_USER)) table[index] |= PAGE_USER;
        return table_from_entry(table[index]);
    }
    if (!create) return NULL;

    uint64_t physical = (uint64_t)pmm_alloc_page();
    uint64_t *new_table = page_table_pointer(physical);
    if (!new_table) {
        pmm_free_page((void *)physical);
        return NULL;
    }
    memset(new_table, 0, 4096);
    uint64_t table_flags = PAGE_PRESENT | PAGE_WRITE;
    if (leaf_flags & PAGE_USER) table_flags |= PAGE_USER;
    table[index] = physical | table_flags;
    return new_table;
}

void vmm_init(void) {
    memset(address_spaces, 0, sizeof(address_spaces));

    kernel_cr3_physical = read_cr3();
    if (!pmm_page_is_allocated(kernel_cr3_physical) ||
        registry_add(address_spaces, MAX_ADDRESS_SPACES, kernel_cr3_physical) != 0) {
        panic("VMM: invalid boot CR3");
    }
    uint64_t *pml4 = page_table_pointer(kernel_cr3_physical);
    if (!pml4) panic("VMM: boot PML4 unavailable");

    uint16_t high_pml4 = (uint16_t)((KERNEL_BASE >> 39) & 0x1FF);
    uint16_t high_pdp = (uint16_t)((KERNEL_BASE >> 30) & 0x1FF);
    uint64_t pdpt_physical = pml4[high_pml4] & ADDRESS_MASK;
    uint64_t *pdpt = page_table_pointer(pdpt_physical);
    if (!pdpt) panic("VMM: boot PDPT unavailable");
    uint64_t pd_physical = pdpt[high_pdp] & ADDRESS_MASK;
    uint64_t *pd = page_table_pointer(pd_physical);
    if (!pd) panic("VMM: boot PD unavailable");

    for (uint64_t i = 0; i < 512; i++) {
        pd[i] = (i * 0x200000ULL) | PAGE_PRESENT | PAGE_WRITE | PAGE_HUGE;
    }
    write_cr3(kernel_cr3_physical);
    KDEBUG("VMM: 1 GiB supervisor-only direct map ready\n");
}

uint64_t vmm_kernel_cr3(void) { return kernel_cr3_physical; }
uint64_t vmm_current_cr3(void) { return read_cr3(); }

uint64_t vmm_create_address_space(void) {
    uint64_t physical = (uint64_t)pmm_alloc_page();
    if (registry_add(address_spaces, MAX_ADDRESS_SPACES, physical) != 0) {
        pmm_free_page((void *)physical);
        return 0;
    }
    uint64_t *new_pml4 = page_table_pointer(physical);
    uint64_t *kernel_pml4 = page_table_pointer(kernel_cr3_physical);
    if (!new_pml4 || !kernel_pml4) {
        registry_remove(address_spaces, MAX_ADDRESS_SPACES, physical);
        pmm_free_page((void *)physical);
        return 0;
    }
    memset(new_pml4, 0, 4096);
    for (uint64_t i = 256; i < 512; i++) new_pml4[i] = kernel_pml4[i];
    return physical;
}

void vmm_activate(uint64_t cr3_physical) {
    uint64_t physical = cr3_physical & ADDRESS_MASK;
    if (!address_space_registered(physical)) {
        kprintf("VMM: activate rejected stale CR3=%p current=%p\n",
                (void *)physical, (void *)read_cr3());
        panic("VMM: attempted to activate stale address space");
    }
    write_cr3(physical);
}

int vmm_map_page_in(uint64_t cr3_physical, uint64_t virtual_address,
                    uint64_t physical_address, uint64_t flags) {
    uint64_t cr3 = cr3_physical & ADDRESS_MASK;
    if (!address_space_registered(cr3)) return -1;
    if ((virtual_address & 0xFFF) || (physical_address & 0xFFF)) return -1;
    if ((flags & PAGE_USER) && virtual_address >= USER_ADDRESS_LIMIT) return -1;
    if ((flags & PAGE_USER) && cr3 == kernel_cr3_physical) return -1;
    uint64_t *pml4 = page_table_pointer(cr3);
    if (!pml4) return -1;
    uint16_t i4 = (virtual_address >> 39) & 0x1FF;
    uint16_t i3 = (virtual_address >> 30) & 0x1FF;
    uint16_t i2 = (virtual_address >> 21) & 0x1FF;
    uint16_t i1 = (virtual_address >> 12) & 0x1FF;

    uint64_t *pdpt = next_table(pml4, i4, flags, 1);
    if (!pdpt) return -1;
    uint64_t *pd = next_table(pdpt, i3, flags, 1);
    if (!pd) return -1;
    uint64_t *pt = next_table(pd, i2, flags, 1);
    if (!pt) return -1;
    if (pt[i1] & PAGE_PRESENT) return -2;
    pt[i1] = (physical_address & ADDRESS_MASK) | flags | PAGE_PRESENT;
    if (cr3 == read_cr3()) invalidate(virtual_address);
    return 0;
}

int vmm_unmap_page_in(uint64_t cr3_physical, uint64_t virtual_address) {
    uint64_t cr3 = cr3_physical & ADDRESS_MASK;
    if (!address_space_registered(cr3)) return -1;
    uint64_t *pml4 = page_table_pointer(cr3);
    if (!pml4) return -1;
    uint16_t i4 = (virtual_address >> 39) & 0x1FF;
    uint16_t i3 = (virtual_address >> 30) & 0x1FF;
    uint16_t i2 = (virtual_address >> 21) & 0x1FF;
    uint16_t i1 = (virtual_address >> 12) & 0x1FF;
    uint64_t *pdpt = next_table(pml4, i4, 0, 0);
    if (!pdpt) return -1;
    uint64_t *pd = next_table(pdpt, i3, 0, 0);
    if (!pd) return -1;
    uint64_t *pt = next_table(pd, i2, 0, 0);
    if (!pt || !(pt[i1] & PAGE_PRESENT)) return -1;
    pt[i1] = 0;
    if (cr3 == read_cr3()) invalidate(virtual_address);
    return 0;
}

int vmm_protect_page_in(uint64_t cr3_physical, uint64_t virtual_address,
                        uint64_t flags) {
    uint64_t cr3 = cr3_physical & ADDRESS_MASK;
    if (!address_space_registered(cr3)) return -1;
    uint64_t *pml4 = page_table_pointer(cr3);
    if (!pml4) return -1;
    uint16_t i4 = (virtual_address >> 39) & 0x1FF;
    uint16_t i3 = (virtual_address >> 30) & 0x1FF;
    uint16_t i2 = (virtual_address >> 21) & 0x1FF;
    uint16_t i1 = (virtual_address >> 12) & 0x1FF;
    uint64_t *pdpt = next_table(pml4, i4, flags, 0);
    if (!pdpt) return -1;
    uint64_t *pd = next_table(pdpt, i3, flags, 0);
    if (!pd) return -1;
    uint64_t *pt = next_table(pd, i2, flags, 0);
    if (!pt || !(pt[i1] & PAGE_PRESENT)) return -1;
    uint64_t physical = pt[i1] & ADDRESS_MASK;
    pt[i1] = physical | (flags & ~ADDRESS_MASK) | PAGE_PRESENT;
    if (cr3 == read_cr3()) invalidate(virtual_address);
    return 0;
}

int vmm_translate(uint64_t cr3_physical, uint64_t virtual_address,
                  uint64_t *physical_out, uint64_t *flags_out) {
    uint64_t cr3 = cr3_physical & ADDRESS_MASK;
    if (!address_space_registered(cr3)) return -1;
    uint64_t *pml4 = page_table_pointer(cr3);
    if (!pml4) return -1;
    uint64_t e4 = pml4[(virtual_address >> 39) & 0x1FF];
    if (!(e4 & PAGE_PRESENT)) return -1;
    uint64_t effective_user = e4 & PAGE_USER;
    uint64_t effective_write = e4 & PAGE_WRITE;
    uint64_t effective_nx = e4 & PAGE_NX;

    uint64_t *pdpt = table_from_entry(e4);
    if (!pdpt) return -1;
    uint64_t e3 = pdpt[(virtual_address >> 30) & 0x1FF];
    if (!(e3 & PAGE_PRESENT)) return -1;
    effective_user &= e3;
    effective_write &= e3;
    effective_nx |= e3 & PAGE_NX;
    if (e3 & PAGE_HUGE) {
        if (physical_out) *physical_out =
            (e3 & 0x000FFFFFC0000000ULL) | (virtual_address & 0x3FFFFFFFULL);
        if (flags_out) {
            uint64_t effective = e3;
            if (!effective_user) effective &= ~PAGE_USER;
            if (!effective_write) effective &= ~PAGE_WRITE;
            if (effective_nx) effective |= PAGE_NX;
            *flags_out = effective;
        }
        return 0;
    }

    uint64_t *pd = table_from_entry(e3);
    if (!pd) return -1;
    uint64_t e2 = pd[(virtual_address >> 21) & 0x1FF];
    if (!(e2 & PAGE_PRESENT)) return -1;
    effective_user &= e2;
    effective_write &= e2;
    effective_nx |= e2 & PAGE_NX;
    if (e2 & PAGE_HUGE) {
        if (physical_out) *physical_out =
            (e2 & 0x000FFFFFFFE00000ULL) | (virtual_address & 0x1FFFFFULL);
        if (flags_out) {
            uint64_t effective = e2;
            if (!effective_user) effective &= ~PAGE_USER;
            if (!effective_write) effective &= ~PAGE_WRITE;
            if (effective_nx) effective |= PAGE_NX;
            *flags_out = effective;
        }
        return 0;
    }

    uint64_t *pt = table_from_entry(e2);
    if (!pt) return -1;
    uint64_t e1 = pt[(virtual_address >> 12) & 0x1FF];
    if (!(e1 & PAGE_PRESENT)) return -1;
    effective_user &= e1;
    effective_write &= e1;
    effective_nx |= e1 & PAGE_NX;
    if (physical_out) *physical_out =
        (e1 & ADDRESS_MASK) | (virtual_address & 0xFFF);
    if (flags_out) {
        uint64_t effective = e1;
        if (!effective_user) effective &= ~PAGE_USER;
        if (!effective_write) effective &= ~PAGE_WRITE;
        if (effective_nx) effective |= PAGE_NX;
        *flags_out = effective;
    }
    return 0;
}

int vmm_user_range_valid(uint64_t cr3_physical, uint64_t address,
                         size_t length, int write_required) {
    if (!length) return 1;
    if (address >= USER_ADDRESS_LIMIT || length > USER_ADDRESS_LIMIT - address) return 0;
    uint64_t first = address & ~0xFFFULL;
    uint64_t last = (address + length - 1) & ~0xFFFULL;
    for (uint64_t page = first;; page += 4096) {
        uint64_t flags;
        if (vmm_translate(cr3_physical, page, NULL, &flags) != 0) return 0;
        if (!(flags & PAGE_USER) || (write_required && !(flags & PAGE_WRITE))) return 0;
        if (page == last) break;
    }
    return 1;
}

int vmm_copy_from_space(uint64_t cr3_physical, void *destination,
                        uint64_t source_user, size_t length) {
    if (!vmm_user_range_valid(cr3_physical, source_user, length, 0)) return -1;
    uint8_t *out = (uint8_t *)destination;
    while (length) {
        uint64_t physical;
        uint64_t flags;
        if (vmm_translate(cr3_physical, source_user, &physical, &flags) != 0) return -1;
        if (flags & PAGE_DEVICE) return -1;
        size_t chunk = 4096 - (size_t)(source_user & 0xFFF);
        if (chunk > length) chunk = length;
        if (!physical_direct_range_valid(physical, chunk)) return -1;
        memcpy(out, (void *)(KERNEL_BASE + physical), chunk);
        out += chunk;
        source_user += chunk;
        length -= chunk;
    }
    return 0;
}

int vmm_copy_to_space(uint64_t cr3_physical, uint64_t destination_user,
                      const void *source, size_t length) {
    if (!vmm_user_range_valid(cr3_physical, destination_user, length, 1)) return -1;
    const uint8_t *in = (const uint8_t *)source;
    while (length) {
        uint64_t physical;
        uint64_t flags;
        if (vmm_translate(cr3_physical, destination_user, &physical, &flags) != 0) return -1;
        if (flags & PAGE_DEVICE) return -1;
        size_t chunk = 4096 - (size_t)(destination_user & 0xFFF);
        if (chunk > length) chunk = length;
        if (!physical_direct_range_valid(physical, chunk)) return -1;
        memcpy((void *)(KERNEL_BASE + physical), in, chunk);
        in += chunk;
        destination_user += chunk;
        length -= chunk;
    }
    return 0;
}

void vmm_map_page(uint64_t virtual_address, uint64_t physical_address,
                  uint16_t flags) {
    int status = vmm_map_page_in(kernel_cr3_physical, virtual_address,
                                 physical_address, flags);
    if (status != 0) panic("VMM: kernel map failed");
}

void vmm_unmap_page(uint64_t virtual_address) {
    vmm_unmap_page_in(kernel_cr3_physical, virtual_address);
}

static void destroy_user_table(uint64_t physical, int level);

static uint64_t clone_user_table(uint64_t source_physical, int level) {
    uint64_t *source = page_table_pointer(source_physical);
    if (!source) return 0;

    uint64_t destination_physical = (uint64_t)pmm_alloc_page();
    uint64_t *destination = page_table_pointer(destination_physical);
    if (!destination) {
        pmm_free_page((void *)destination_physical);
        return 0;
    }
    memset(destination, 0, 4096);

    for (uint64_t index = 0; index < 512; index++) {
        uint64_t entry = source[index];
        if (!(entry & PAGE_PRESENT)) continue;
        if (entry & PAGE_HUGE) {
            destroy_user_table(destination_physical, level);
            return 0;
        }
        uint64_t preserved_flags = entry & ~ADDRESS_MASK;
        if (level == 1) {
            if (entry & PAGE_DEVICE) {
                destination[index] = entry;
            } else {
                uint64_t source_page = entry & ADDRESS_MASK;
                if (!pmm_page_is_allocated(source_page) ||
                    !physical_direct_range_valid(source_page, 4096)) {
                    destroy_user_table(destination_physical, level);
                    return 0;
                }
                uint64_t page_physical = (uint64_t)pmm_alloc_page();
                if (!page_physical) {
                    destroy_user_table(destination_physical, level);
                    return 0;
                }
                memcpy((void *)(KERNEL_BASE + page_physical),
                       (void *)(KERNEL_BASE + source_page), 4096);
                destination[index] = page_physical | preserved_flags;
            }
        } else {
            uint64_t child = clone_user_table(entry & ADDRESS_MASK, level - 1);
            if (!child) {
                destroy_user_table(destination_physical, level);
                return 0;
            }
            destination[index] = child | preserved_flags;
        }
    }
    return destination_physical;
}

uint64_t vmm_clone_address_space(uint64_t source_cr3) {
    uint64_t source_physical = source_cr3 & ADDRESS_MASK;
    if (!address_space_registered(source_physical)) return 0;
    uint64_t destination_cr3 = vmm_create_address_space();
    if (!destination_cr3) return 0;
    uint64_t *source = page_table_pointer(source_physical);
    uint64_t *destination = page_table_pointer(destination_cr3);
    if (!source || !destination) {
        vmm_destroy_address_space(destination_cr3);
        return 0;
    }
    for (uint64_t index = 0; index < 256; index++) {
        uint64_t entry = source[index];
        if (!(entry & PAGE_PRESENT)) continue;
        if (entry & PAGE_HUGE) {
            vmm_destroy_address_space(destination_cr3);
            return 0;
        }
        uint64_t child = clone_user_table(entry & ADDRESS_MASK, 3);
        if (!child) {
            vmm_destroy_address_space(destination_cr3);
            return 0;
        }
        destination[index] = child | (entry & ~ADDRESS_MASK);
    }
    return destination_cr3;
}

static void destroy_user_table(uint64_t physical, int level) {
    physical &= ADDRESS_MASK;
    uint64_t *table = page_table_pointer(physical);
    if (!table) {
        KDEBUG("VMM: skipped stale user page table %p\n", (void *)physical);
        return;
    }
    for (uint64_t index = 0; index < 512; index++) {
        uint64_t entry = table[index];
        if (!(entry & PAGE_PRESENT)) continue;
        if (entry & PAGE_HUGE) {
            KDEBUG("VMM: skipped unexpected user huge page\n");
            continue;
        }
        uint64_t child = entry & ADDRESS_MASK;
        if (level == 1) {
            if (!(entry & PAGE_DEVICE) && pmm_page_is_allocated(child))
                pmm_free_page((void *)child);
        } else {
            destroy_user_table(child, level - 1);
        }
        table[index] = 0;
    }
    if (pmm_page_is_allocated(physical)) pmm_free_page((void *)physical);
}

void vmm_destroy_address_space(uint64_t cr3_physical) {
    uint64_t physical = cr3_physical & ADDRESS_MASK;
    if (physical == kernel_cr3_physical || !physical) return;
    if (!address_space_registered(physical)) {
        KDEBUG("VMM: duplicate/stale address-space destroy %p ignored\n",
               (void *)physical);
        return;
    }
    if (physical == read_cr3()) {
        kprintf("VMM: refused to destroy active CR3=%p\n", (void *)physical);
        return;
    }
    uint64_t *pml4 = page_table_pointer(physical);
    if (!pml4) {
        registry_remove(address_spaces, MAX_ADDRESS_SPACES, physical);
        return;
    }
    for (uint64_t index = 0; index < 256; index++) {
        uint64_t entry = pml4[index];
        if (entry & PAGE_PRESENT) destroy_user_table(entry & ADDRESS_MASK, 3);
        pml4[index] = 0;
    }
    registry_remove(address_spaces, MAX_ADDRESS_SPACES, physical);
    if (pmm_page_is_allocated(physical)) pmm_free_page((void *)physical);
}

static uint64_t count_user_table(uint64_t table_physical, int level) {
    uint64_t *table = page_table_pointer(table_physical);
    if (!table) return 0;
    uint64_t count = 0;
    for (uint64_t index = 0; index < 512; index++) {
        uint64_t entry = table[index];
        if (!(entry & PAGE_PRESENT) || !(entry & PAGE_USER)) continue;
        if (level == 1) {
            if (!(entry & PAGE_DEVICE)) count++;
        } else if (entry & PAGE_HUGE) {
            count += level == 3 ? 262144ULL : 512ULL;
        } else {
            count += count_user_table(entry & ADDRESS_MASK, level - 1);
        }
    }
    return count;
}

uint64_t vmm_count_user_pages(uint64_t cr3_physical) {
    uint64_t physical = cr3_physical & ADDRESS_MASK;
    if (!address_space_registered(physical)) return 0;
    uint64_t *pml4 = page_table_pointer(physical);
    if (!pml4) return 0;
    uint64_t count = 0;
    for (uint64_t index = 0; index < 256; index++) {
        uint64_t entry = pml4[index];
        if ((entry & (PAGE_PRESENT | PAGE_USER)) == (PAGE_PRESENT | PAGE_USER))
            count += count_user_table(entry & ADDRESS_MASK, 3);
    }
    return count;
}
