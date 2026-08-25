#include "include/build_config.h"
#include <stddef.h>
#include <stdint.h>
#include "include/kstring.h"
#include "include/oplock.h"

static int vmm_map_page_in_locked(uint64_t cr3_physical, uint64_t virtual_address,
                                  uint64_t physical_address, uint64_t flags);
#include "include/boot.h"
#include "include/pmm.h"
#include "include/smp.h"
#include "include/vmm.h"

#define ADDRESS_MASK 0x000FFFFFFFFFF000ULL
#define DIRECT_MAP_SIZE PMM_DIRECT_MAP_LIMIT
#define MAX_ADDRESS_SPACES 256

extern void panic(const char *msg) __attribute__((noreturn));
extern void kprintf(const char *fmt, ...);
/* Commits a reserved page the kernel is about to touch (see process.c). */
extern int process_commit_area(uint64_t fault_address);
extern int process_grow_user_stack(uint64_t fault_address);

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
    return (void *)(DIRECT_MAP_BASE + physical);
}

/*
 * The other direction, and it has to accept two windows.
 *
 * Most callers hand back something vmm_phys_to_virt() gave them, which is in
 * the direct map. But the DMA drivers hand it a *static* buffer -- rtl8139's
 * receive ring and transmit slots are plain arrays in the kernel image -- and
 * those live in the image's own mapping. That one is no longer "physical plus
 * KERNEL_BASE": Limine loads the image wherever it likes and reports where,
 * so the offset between the two comes from the loader rather than a constant.
 */
uint64_t vmm_virt_to_phys_direct(const void *virtual_address) {
    const struct boot_info *boot = boot_info();
    uint64_t value = (uint64_t)virtual_address;
    uint64_t physical;
    if (value >= DIRECT_MAP_BASE && value < DIRECT_MAP_BASE + DIRECT_MAP_SIZE) {
        physical = value - DIRECT_MAP_BASE;
    } else if (value >= boot->kernel_virtual_base &&
               value < boot->kernel_virtual_base + boot->kernel_size) {
        physical = value - boot->kernel_virtual_base + boot->kernel_physical_base;
    } else {
        panic("VMM: address is not in direct map");
    }
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
    return (uint64_t *)(DIRECT_MAP_BASE + physical);
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

/*
 * Put write-combining where a page table entry can ask for it.
 *
 * The PAT is eight slots, and a page selects one with three bits spread across
 * its entry. The first four slots keep their power-on meanings so that every
 * existing mapping keeps behaving exactly as it did; only slot 4 -- which no
 * mapping selects until one sets bit 7 -- is changed from write-back to
 * write-combining. That is the same slot Linux repurposes, and for the same
 * reason: it is the one that can be changed without auditing everything else.
 */
#define IA32_PAT_MSR 0x277U
#define CPUID_FEATURES_LEAF 1U
#define CPUID_EDX_PAT (1U << 16)
/* PA0..PA3 as the processor leaves them, PA4 write-combining, PA5..PA7 as
   they were: WB, WT, UC-, UC, WC, WT, UC-, UC. */
#define PAT_WITH_WRITE_COMBINING 0x0007040100070406ULL

static int write_combining;

static inline void write_msr(uint32_t msr, uint64_t value) {
    __asm__ volatile("wrmsr" : : "c"(msr), "a"((uint32_t)value),
                                 "d"((uint32_t)(value >> 32)));
}

static void configure_page_attributes(void) {
    uint32_t a = 0, b = 0, c = 0, d = 0;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                             : "a"(CPUID_FEATURES_LEAF), "c"(0));
    if (!(d & CPUID_EDX_PAT)) return;
    write_msr(IA32_PAT_MSR, PAT_WITH_WRITE_COMBINING);
    write_combining = 1;
}

int vmm_write_combining_available(void) { return write_combining; }

void vmm_init(void) {
    memset(address_spaces, 0, sizeof(address_spaces));
    /* Before any mapping is made, so nothing is ever mapped through a slot
       whose meaning is about to change under it. */
    configure_page_attributes();

    kernel_cr3_physical = read_cr3();

    /*
     * Everything up to the point the direct map exists has to be reached
     * through the loader's window instead, because page_table_pointer() now
     * answers with an address that is not mapped yet. `early` is that window:
     * Limine's higher-half direct map, which covers every page the tables
     * below are allocated from.
     *
     * The kernel image's own mapping is left exactly as the loader made it.
     * The framebuffer and device windows sit in the gigabyte above it, which
     * Limine leaves empty, so there is nothing here to take back first.
     */
    const uint64_t hhdm = boot_info()->hhdm_offset;
#define early(physical) ((uint64_t *)(hhdm + ((physical) & ADDRESS_MASK)))
    uint64_t *pml4 = early(kernel_cr3_physical);

    /*
     * The direct map, in a PML4 entry of its own.
     *
     * Only as much of it as there is RAM to map: the ceiling is what the
     * address space allows, not what has to be built. A gigabyte of physical
     * memory costs one page of directory here, so a machine with four costs
     * sixteen kilobytes.
     */
    uint16_t direct_pml4 = (uint16_t)((DIRECT_MAP_BASE >> 39) & 0x1FF);
    uint64_t direct_pdpt_physical = (uint64_t)pmm_alloc_page();
    if (!direct_pdpt_physical) panic("VMM: direct map PDPT unavailable");
    uint64_t *direct_pdpt = early(direct_pdpt_physical);
    memset(direct_pdpt, 0, 4096);

    uint64_t mapped = pmm_managed_limit();
    if (mapped > DIRECT_MAP_SIZE) mapped = DIRECT_MAP_SIZE;
    for (uint64_t gigabyte = 0; gigabyte * 0x40000000ULL < mapped; gigabyte++) {
        uint64_t table_physical = (uint64_t)pmm_alloc_page();
        if (!table_physical) panic("VMM: direct map directory unavailable");
        uint64_t *table = early(table_physical);
        memset(table, 0, 4096);
        for (uint64_t i = 0; i < 512; i++) {
            uint64_t frame = gigabyte * 0x40000000ULL + i * 0x200000ULL;
            table[i] = frame | PAGE_PRESENT | PAGE_WRITE | PAGE_HUGE;
        }
        direct_pdpt[gigabyte] = table_physical | PAGE_PRESENT | PAGE_WRITE;
    }
    pml4[direct_pml4] = direct_pdpt_physical | PAGE_PRESENT | PAGE_WRITE;
    write_cr3(kernel_cr3_physical);
#undef early

    /* The direct map answers now, so the ordinary helpers can be used. */
    if (!pmm_page_is_allocated(kernel_cr3_physical) ||
        registry_add(address_spaces, MAX_ADDRESS_SPACES, kernel_cr3_physical) != 0) {
        panic("VMM: invalid boot CR3");
    }
    pml4 = page_table_pointer(kernel_cr3_physical);
    if (!pml4) panic("VMM: boot PML4 unavailable");

    /*
     * The heap's own PML4 entry, created here so that every address space
     * cloned later inherits it. Everything below it is filled in on demand by
     * heap_grow(), and those tables hang off this one entry, so they are
     * visible everywhere without any further copying.
     */
    uint16_t heap_pml4 = (uint16_t)((HEAP_VIRTUAL_BASE >> 39) & 0x1FF);
    if (!(pml4[heap_pml4] & PAGE_PRESENT)) {
        uint64_t heap_pdpt = (uint64_t)pmm_alloc_page();
        uint64_t *table = page_table_pointer(heap_pdpt);
        if (!table) panic("VMM: heap PDPT unavailable");
        memset(table, 0, 4096);
        pml4[heap_pml4] = heap_pdpt | PAGE_PRESENT | PAGE_WRITE;
    }

    write_cr3(kernel_cr3_physical);
    KDEBUG("VMM: %u MiB direct map ready, %u MiB of room\n",
           (unsigned)(mapped / (1024 * 1024)),
           (unsigned)(DIRECT_MAP_SIZE / (1024 * 1024)));
}

uint64_t vmm_kernel_cr3(void) { return kernel_cr3_physical; }

uint64_t vmm_map_device(uint64_t physical, uint64_t bytes) {
    static uint64_t arena_used = 0;
    if (!physical || !bytes) return 0;

    uint64_t page_offset = physical & 0xFFFULL;
    uint64_t first = physical - page_offset;
    uint64_t span = (bytes + page_offset + 0xFFFULL) & ~0xFFFULL;
    if (DEVICE_MMIO_ARENA_OFFSET + arena_used + span > DEVICE_MMIO_VIRTUAL_BYTES)
        return 0;

    uint64_t base = DEVICE_MMIO_VIRTUAL_BASE + DEVICE_MMIO_ARENA_OFFSET + arena_used;
    for (uint64_t offset = 0; offset < span; offset += 4096ULL) {
        if (vmm_map_page_in(kernel_cr3_physical, base + offset, first + offset,
                            PAGE_WRITE | PAGE_DEVICE | PAGE_UNCACHED | PAGE_NX) != 0)
            return 0;
    }
    arena_used += span;
    return base + page_offset;
}
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

/*
 * Guarded because two threads of one process can now be inside the kernel at
 * once, and a mapping walks and extends tables they share. The intermediate
 * tables are the hazard rather than the leaf: two processors finding the same
 * directory missing would both create one, and one of the two would be lost
 * along with everything mapped through it.
 */
int vmm_map_page_in(uint64_t cr3_physical, uint64_t virtual_address,
                    uint64_t physical_address, uint64_t flags) {
    oplock_enter();
    int status = vmm_map_page_in_locked(cr3_physical, virtual_address,
                                        physical_address, flags);
    oplock_leave();
    return status;
}

static int vmm_map_page_in_locked(uint64_t cr3_physical, uint64_t virtual_address,
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
    smp_flush_address_space(cr3);
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
    smp_flush_address_space(cr3);
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
        if (vmm_translate(cr3_physical, page, NULL, &flags) != 0) {
            /* Reserved but not yet touched: userspace would have faulted the
               page in here, so do it for the kernel before giving up.
               Both kinds of lazy page have to be handled, because the kernel
               writes into both: an anonymous mapping's first touch, and the
               main stack's next page down. The stack one is not hypothetical --
               a signal frame is built *below* the stack pointer and written by
               hand rather than through a fault, so a process signalled while
               its stack pointer sat near a page boundary had the frame land on
               a page it had never reached. That failed the copy, and the
               delivery path answers a failed copy with a silent SIGSEGV. */
            if (cr3_physical != read_cr3() ||
                (!process_commit_area(page) && !process_grow_user_stack(page)) ||
                vmm_translate(cr3_physical, page, NULL, &flags) != 0) return 0;
        }
        /* A copy-on-write page is logically writable even though the hardware
           entry is read-only: the write is allowed, it just has to break the
           sharing first. Callers that actually store into the page do that via
           vmm_copy_to_space(); callers that only validate must not reject it,
           or a post-fork read() into a buffer would fail with EFAULT. */
        if (!(flags & PAGE_USER) ||
            (write_required && !(flags & (PAGE_WRITE | PAGE_COW)))) return 0;
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
        memcpy(out, (void *)(DIRECT_MAP_BASE + physical), chunk);
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
        /* The kernel is about to store into this page, which is exactly the
           event the copy-on-write mapping exists to intercept. Userspace would
           have taken a fault here; do the same work inline, then re-translate
           because the page may now live somewhere else. */
        if (flags & PAGE_COW) {
            if (vmm_handle_cow_fault(cr3_physical, destination_user & ~0xFFFULL) != 0)
                return -1;
            if (vmm_translate(cr3_physical, destination_user, &physical, &flags) != 0)
                return -1;
        }
        if (!(flags & PAGE_WRITE)) return -1;
        size_t chunk = 4096 - (size_t)(destination_user & 0xFFF);
        if (chunk > length) chunk = length;
        if (!physical_direct_range_valid(physical, chunk)) return -1;
        memcpy((void *)(DIRECT_MAP_BASE + physical), in, chunk);
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
                /*
                 * A MAP_SHARED page is inherited as-is: both processes keep
                 * writing to the same memory, which is the entire contract.
                 * Marking it copy-on-write would give the child a private copy
                 * on its first store and silently break wl_shm-style buffer
                 * sharing.
                 */
                if ((entry & PAGE_SHARED) && pmm_page_ref(source_page) == 0) {
                    destination[index] = entry;
                    continue;
                }
                /*
                 * Share rather than copy. A writable page becomes read-only and
                 * copy-on-write in *both* address spaces, so whichever side
                 * writes first takes the fault and gets its own copy. A page
                 * that was already read-only needs no COW marking -- a write to
                 * it was a fault before the fork and still is -- but it does
                 * need the reference, because both owners will free it.
                 *
                 * If the reference count saturates we fall back to copying,
                 * which is what this code did unconditionally before.
                 */
                if (pmm_page_ref(source_page) == 0) {
                    uint64_t shared_flags = preserved_flags;
                    if (shared_flags & PAGE_WRITE) {
                        shared_flags = (shared_flags & ~PAGE_WRITE) | PAGE_COW;
                        source[index] = source_page | shared_flags;
                    }
                    destination[index] = source_page | shared_flags;
                } else {
                    uint64_t page_physical = (uint64_t)pmm_alloc_page();
                    if (!page_physical) {
                        destroy_user_table(destination_physical, level);
                        return 0;
                    }
                    memcpy((void *)(DIRECT_MAP_BASE + page_physical),
                           (void *)(DIRECT_MAP_BASE + source_page), 4096);
                    destination[index] = page_physical | preserved_flags;
                }
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
    /*
     * The clone cleared PAGE_WRITE on the *source*'s shared pages, and the
     * source is the running process, so its TLB still holds writable entries
     * for them. Reloading CR3 flushes the lot; doing it once here is cheaper
     * and far less error-prone than an invlpg per page during the walk.
     */
    if (source_physical == read_cr3()) write_cr3(source_physical);
    /* And every other processor running a thread of the same process, which
       would otherwise keep writing straight through the sharing. */
    smp_flush_address_space(source_physical);
    return destination_cr3;
}

/*
 * Break a copy-on-write page after a write fault. Returns 0 when the fault was
 * handled and the instruction should be retried, -1 when it was not a COW fault
 * and the caller should carry on to the signal path.
 */
int vmm_handle_cow_fault(uint64_t cr3_physical, uint64_t virtual_address) {
    uint64_t cr3 = cr3_physical & ADDRESS_MASK;
    if (!address_space_registered(cr3)) return -1;
    if (virtual_address >= USER_ADDRESS_LIMIT) return -1;

    uint64_t *pml4 = page_table_pointer(cr3);
    if (!pml4) return -1;
    uint64_t *pdpt = next_table(pml4, (virtual_address >> 39) & 0x1FF, 0, 0);
    if (!pdpt) return -1;
    uint64_t *pd = next_table(pdpt, (virtual_address >> 30) & 0x1FF, 0, 0);
    if (!pd) return -1;
    uint64_t *pt = next_table(pd, (virtual_address >> 21) & 0x1FF, 0, 0);
    if (!pt) return -1;

    uint16_t index = (virtual_address >> 12) & 0x1FF;
    uint64_t entry = pt[index];
    /* Already private and writable: another processor running a thread of the
       same process broke this page first, and this fault was taken on a
       translation that has since been replaced. Retrying is the whole fix. */
    if ((entry & (PAGE_PRESENT | PAGE_USER | PAGE_WRITE | PAGE_COW)) ==
        (PAGE_PRESENT | PAGE_USER | PAGE_WRITE)) {
        if (cr3 == read_cr3()) invalidate(virtual_address);
        return 0;
    }
    if ((entry & (PAGE_PRESENT | PAGE_COW | PAGE_USER)) !=
        (PAGE_PRESENT | PAGE_COW | PAGE_USER)) return -1;

    uint64_t physical = entry & ADDRESS_MASK;
    uint64_t flags = (entry & ~ADDRESS_MASK & ~PAGE_COW & ~PAGE_FILEBACKED) | PAGE_WRITE;

    /* Sole remaining owner: no copy needed, just hand the page back its write
       permission. This is the common case once siblings have exited or exec'd.
       Never for a file-backed frame -- see PAGE_FILEBACKED. */
    if (!(entry & PAGE_FILEBACKED) && pmm_page_refcount(physical) <= 1) {
        pt[index] = physical | flags;
    } else {
        if (!physical_direct_range_valid(physical, 4096)) return -1;
        uint64_t copy = (uint64_t)pmm_alloc_page();
        if (!copy || !physical_direct_range_valid(copy, 4096)) {
            if (copy) pmm_free_page((void *)copy);
            return -1;
        }
        memcpy((void *)(DIRECT_MAP_BASE + copy), (void *)(DIRECT_MAP_BASE + physical), 4096);
        pt[index] = copy | flags;
        /* The other processors first, then the reference: one of them could
           still be writing through the old translation, and this is the call
           that may hand the frame back to the allocator. */
        smp_flush_address_space(cr3);
        /* Drops this space's reference to the page it no longer maps. */
        pmm_free_page((void *)physical);
    }
    if (cr3 == read_cr3()) invalidate(virtual_address);
    return 0;
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
