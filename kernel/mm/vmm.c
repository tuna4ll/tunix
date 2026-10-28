#include <tunix/build_config.h>
#include <stddef.h>
#include <stdint.h>
#include <tunix/kstring.h>
#include <tunix/lock.h>
#include <tunix/percpu.h>

static int vmm_map_page_in_locked(uint64_t cr3_physical, uint64_t virtual_address,
                                  uint64_t physical_address, uint64_t flags);
static uint64_t map_device_locked(uint64_t physical, uint64_t bytes, uint64_t cache);
#include <tunix/boot.h>
#include <tunix/heap.h>
#include <tunix/pmm.h>
#include <tunix/smp.h>
#include <tunix/vmm.h>
#include <tunix/vmm_arch.h>
#include <tunix/kprintf.h>

#define ADDRESS_MASK     PTE_ADDRESS_MASK
#define KERNEL_HALF_BASE 0xFFFF800000000000ULL

static int kernel_mapping(uint64_t virtual_address) { return virtual_address >= KERNEL_HALF_BASE; }

extern int process_commit_area(uint64_t fault_address);
extern int process_grow_user_stack(uint64_t fault_address);

#if TUNIX_DEBUG_LOGS
#define KDEBUG(...) kprintf(__VA_ARGS__)
#else
#define KDEBUG(...) \
    do { \
    } while (0)
#endif

static struct lock tables_lock = LOCK_INITIALIZER("page tables", LOCK_RANK_KERNEL_MAP);
static uint64_t kernel_cr3_physical;
struct vmm_space {
    uint64_t cr3;
    int64_t pages[VMM_USAGE_KINDS];
};

static struct vmm_space kernel_space;
static struct vmm_space *boot_space_slots[512];
static struct vmm_space **space_slots = boot_space_slots;
static uint64_t space_capacity = 512;
static uint64_t space_count;

static int physical_direct_range_valid(uint64_t physical, size_t length) {
    if (!length) return physical < DIRECT_MAP_BYTES;
    if (physical >= DIRECT_MAP_BYTES || length > DIRECT_MAP_BYTES - physical) return 0;
    return pmm_physical_range_managed(physical, (uint64_t)length);
}

static void report_bad_physical(const char *operation, uint64_t physical, const void *caller) {
    kprintf("VMM: %s physical=%p caller=%p managed_limit=%p direct_limit=%p\n", operation,
            (void *)physical, (void *)caller, (void *)pmm_managed_limit(),
            (void *)DIRECT_MAP_BYTES);
}

void *vmm_phys_to_virt(uint64_t physical) {
    if (!physical_direct_range_valid(physical, 1)) {
        report_bad_physical("phys_to_virt rejected", physical, __builtin_return_address(0));
        panic("VMM: invalid physical address");
    }
    return (void *)(DIRECT_MAP_BASE + physical);
}

uint64_t vmm_virt_to_phys_direct(const void *virtual_address) {
    const struct boot_info *boot = boot_info();
    uint64_t value = (uint64_t)virtual_address;
    uint64_t physical;
    if (value >= DIRECT_MAP_BASE && value < DIRECT_MAP_BASE + DIRECT_MAP_BYTES) {
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

uint64_t vmm_dma_physical(const void *pointer, uint64_t length) {
    const struct boot_info *boot = boot_info();
    uint64_t value = (uint64_t)(uintptr_t)pointer;

    if (value >= DIRECT_MAP_BASE && value < DIRECT_MAP_BASE + DIRECT_MAP_BYTES) {
        uint64_t physical = value - DIRECT_MAP_BASE;
        return pmm_physical_range_managed(physical, length) ? physical : 0;
    }
    if (value >= boot->kernel_virtual_base &&
        value - boot->kernel_virtual_base < boot->kernel_size) {
        uint64_t offset = value - boot->kernel_virtual_base;
        if (length > boot->kernel_size - offset) return 0;
        return boot->kernel_physical_base + offset;
    }
    return 0;
}

static uint64_t space_home(uint64_t cr3, uint64_t capacity) {
    return ((cr3 >> 12) * 0x9E3779B97F4A7C15ULL >> 32) & (capacity - 1);
}

static struct vmm_space *space_known(uint64_t cr3) {
    uint64_t mask = space_capacity - 1;
    for (uint64_t at = space_home(cr3, space_capacity);; at = (at + 1) & mask) {
        if (!space_slots[at]) return NULL;
        if (space_slots[at]->cr3 == cr3) return space_slots[at];
    }
}

static void space_place(struct vmm_space **slots, uint64_t capacity, struct vmm_space *space) {
    uint64_t at = space_home(space->cr3, capacity);
    while (slots[at]) at = (at + 1) & (capacity - 1);
    slots[at] = space;
}

static int space_add(struct vmm_space *space) {
    if (!space || !space->cr3 || space_known(space->cr3)) return -1;
    if ((space_count + 1) * 2 > space_capacity) {
        uint64_t capacity = space_capacity * 2;
        uint64_t pages = capacity * sizeof(*space_slots) / 4096ULL;
        uint64_t physical = (uint64_t)pmm_alloc_pages(pages, 4096);
        struct vmm_space **slots =
            physical ? (struct vmm_space **)vmm_phys_to_virt(physical) : NULL;
        if (!slots) {
            if (physical) pmm_free_pages((void *)physical, pages);
            return -1;
        }
        memset(slots, 0, capacity * sizeof(*slots));
        for (uint64_t index = 0; index < space_capacity; index++)
            if (space_slots[index]) space_place(slots, capacity, space_slots[index]);
        struct vmm_space **old = space_slots;
        uint64_t old_pages = space_capacity * sizeof(*space_slots) / 4096ULL;
        space_slots = slots;
        space_capacity = capacity;
        if (old != boot_space_slots)
            pmm_free_pages((void *)vmm_virt_to_phys_direct(old), old_pages);
    }
    space_place(space_slots, space_capacity, space);
    space_count++;
    return 0;
}

static struct vmm_space *last_space;

static void space_remove(uint64_t cr3) {
    if (!cr3) return;
    uint64_t mask = space_capacity - 1;
    uint64_t at = space_home(cr3, space_capacity);
    while (!space_slots[at] || space_slots[at]->cr3 != cr3) {
        if (!space_slots[at]) return;
        at = (at + 1) & mask;
    }
    if (last_space == space_slots[at]) last_space = NULL;
    space_slots[at] = NULL;
    space_count--;
    for (uint64_t next = (at + 1) & mask; space_slots[next]; next = (next + 1) & mask) {
        uint64_t home = space_home(space_slots[next]->cr3, space_capacity);
        if (((next - home) & mask) >= ((next - at) & mask)) {
            space_slots[at] = space_slots[next];
            space_slots[next] = NULL;
            at = next;
        }
    }
}

static struct vmm_space *address_space_registered(uint64_t cr3_physical) {
    uint64_t physical = cr3_physical & ADDRESS_MASK;
    if (!physical) return NULL;
    if (last_space && last_space->cr3 == physical) return last_space;
    struct vmm_space *space = space_known(physical);
    if (space) last_space = space;
    return space;
}

static int usage_kind(uint64_t entry) {
    if (!pte_present(entry)) return -1;
    uint64_t flags = pte_flags(entry);
    if (!(flags & PAGE_USER) || (flags & PAGE_DEVICE)) return -1;
    if (flags & PAGE_SHARED) return VMM_USAGE_SHMEM;
    if (flags & PAGE_FILEBACKED) return VMM_USAGE_FILE;
    return VMM_USAGE_ANON;
}

static void account(struct vmm_space *space, uint64_t old_entry, uint64_t new_entry) {
    if (!space || space == &kernel_space) return;
    int kind = usage_kind(old_entry);
    if (kind >= 0) space->pages[kind]--;
    kind = usage_kind(new_entry);
    if (kind >= 0) space->pages[kind]++;
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
    if (!pte_present(entry) || pte_huge(entry)) return NULL;
    return page_table_pointer(pte_address(entry));
}

static uint64_t *next_table(uint64_t *table, uint16_t index, uint64_t leaf_flags, int create) {
    if (!table) return NULL;
    uint64_t entry = table[index];
    if (pte_present(entry)) {
        if (pte_huge(entry)) return NULL;
        pte_table_grant_user(&table[index], leaf_flags);
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
    table[index] = pte_table(physical, table_flags);
    return new_table;
}

#if defined(__x86_64__)
#define IA32_PAT_MSR             0x277U
#define CPUID_FEATURES_LEAF      1U
#define CPUID_EDX_PAT            (1U << 16)
#define PAT_WITH_WRITE_COMBINING 0x0007040100070406ULL
#define CPUID_EXTENDED_LEAF      7U
#define CPUID_EBX_SMEP           (1U << 7)
#define CPUID_EBX_SMAP           (1U << 20)
#define CR4_SMEP                 (1ULL << 20)
#define CR4_SMAP                 (1ULL << 21)

static int write_combining;
static int user_access_guard;
static void configure_page_attributes(void);
static void guard_user_pages(void);

static inline void write_msr(uint32_t msr, uint64_t value) {
    __asm__ volatile("wrmsr" : : "c"(msr), "a"((uint32_t)value), "d"((uint32_t)(value >> 32)));
}

void vmm_configure_processor(void) {
    configure_page_attributes();
    guard_user_pages();
}

static void configure_page_attributes(void) {
    uint32_t a = 0, b = 0, c = 0, d = 0;
    __asm__ volatile("cpuid"
                     : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                     : "a"(CPUID_FEATURES_LEAF), "c"(0));
    if (!(d & CPUID_EDX_PAT)) return;
    write_msr(IA32_PAT_MSR, PAT_WITH_WRITE_COMBINING);
    write_combining = 1;
}

static void guard_user_pages(void) {
    uint32_t a = 0, b = 0, c = 0, d = 0;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0U), "c"(0));
    if (a < CPUID_EXTENDED_LEAF) return;
    __asm__ volatile("cpuid"
                     : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                     : "a"(CPUID_EXTENDED_LEAF), "c"(0));
    uint64_t wanted = 0;
    if (b & CPUID_EBX_SMEP) wanted |= CR4_SMEP;
    if (b & CPUID_EBX_SMAP) wanted |= CR4_SMAP;
    if (!wanted) return;
    if (wanted & CR4_SMAP) __asm__ volatile("clac" ::: "memory");
    uint64_t cr4;
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    __asm__ volatile("mov %0, %%cr4" : : "r"(cr4 | wanted) : "memory");
    user_access_guard = (int)(wanted >> 20);
}

int vmm_user_access_guard(void) { return user_access_guard; }

void vmm_close_user_access(void) {
    if (user_access_guard & 2) __asm__ volatile("clac" ::: "memory");
}
#else
static int write_combining = 1;

void vmm_configure_processor(void) {}

static void configure_page_attributes(void) {}

static void guard_user_pages(void) {}

int vmm_user_access_guard(void) { return 1 | (aarch64_privileged_access_never() ? 2 : 0); }

void vmm_close_user_access(void) {}
#endif

int vmm_write_combining_available(void) { return write_combining; }

void vmm_init(void) {
    configure_page_attributes();
    guard_user_pages();
    int guard = vmm_user_access_guard();
    if (guard)
        kprintf("VMM: user pages guarded from the kernel (%s)\n",
                guard == 3       ? "execute, access"
                    : guard == 2 ? "access"
                                 : "execute");

    kernel_cr3_physical = vmm_arch_read_root();

    const uint64_t hhdm = boot_info()->hhdm_offset;
#define early(physical) ((uint64_t *)(hhdm + ((physical) & ADDRESS_MASK)))
    uint64_t *pml4 = early(kernel_cr3_physical);

    uint64_t slots[DIRECT_MAP_BYTES >> 39] = {0};
    uint64_t mapped = pmm_managed_limit();
    for (uint64_t gigabyte = 0; gigabyte * 0x40000000ULL < mapped; gigabyte++) {
        uint64_t base = gigabyte * 0x40000000ULL;
        int wanted = 0;
        for (uint64_t i = 0; i < 512 && !wanted; i++)
            wanted = vmm_arch_direct_map_wanted(base + i * 0x200000ULL);
        if (!wanted) continue;
        uint64_t slot = base >> 39;
        if (!slots[slot]) {
            slots[slot] = (uint64_t)pmm_alloc_page();
            if (!slots[slot]) panic("VMM: direct map PDPT unavailable");
            memset(early(slots[slot]), 0, 4096);
        }
        uint64_t table_physical = (uint64_t)pmm_alloc_page();
        if (!table_physical) panic("VMM: direct map directory unavailable");
        uint64_t *table = early(table_physical);
        memset(table, 0, 4096);
        for (uint64_t i = 0; i < 512; i++) {
            uint64_t frame = base + i * 0x200000ULL;
            if (vmm_arch_direct_map_wanted(frame))
                table[i] = pte_block(frame, PAGE_PRESENT | PAGE_WRITE);
        }
        early(slots[slot])[gigabyte & 0x1FF] = pte_table(table_physical, PAGE_PRESENT | PAGE_WRITE);
    }
    uint16_t first_slot = (uint16_t)((DIRECT_MAP_BASE >> 39) & 0x1FF);
    for (uint64_t slot = 0; slot < (DIRECT_MAP_BYTES >> 39); slot++)
        if (slots[slot])
            pml4[first_slot + slot] = pte_table(slots[slot], PAGE_PRESENT | PAGE_WRITE);
    vmm_arch_write_root(kernel_cr3_physical);
#undef early
    pmm_use_direct_map(DIRECT_MAP_BASE);

    kernel_space.cr3 = kernel_cr3_physical;
    if (!pmm_page_is_allocated(kernel_cr3_physical) || space_add(&kernel_space) != 0) {
        panic("VMM: invalid boot CR3");
    }
    pml4 = page_table_pointer(kernel_cr3_physical);
    if (!pml4) panic("VMM: boot PML4 unavailable");

    uint16_t device_pml4 = (uint16_t)((DEVICE_ARENA_BASE >> 39) & 0x1FF);
    if (!pte_present(pml4[device_pml4])) {
        uint64_t device_pdpt = (uint64_t)pmm_alloc_page();
        uint64_t *table = page_table_pointer(device_pdpt);
        if (!table) panic("VMM: device PDPT unavailable");
        memset(table, 0, 4096);
        pml4[device_pml4] = pte_table(device_pdpt, PAGE_PRESENT | PAGE_WRITE);
    }
    uint16_t heap_pml4 = (uint16_t)((HEAP_VIRTUAL_BASE >> 39) & 0x1FF);
    if (!pte_present(pml4[heap_pml4])) {
        uint64_t heap_pdpt = (uint64_t)pmm_alloc_page();
        uint64_t *table = page_table_pointer(heap_pdpt);
        if (!table) panic("VMM: heap PDPT unavailable");
        memset(table, 0, 4096);
        pml4[heap_pml4] = pte_table(heap_pdpt, PAGE_PRESENT | PAGE_WRITE);
    }

    vmm_arch_write_root(kernel_cr3_physical);
    KDEBUG("VMM: %u MiB direct map ready, %u MiB of room\n", (unsigned)(mapped / (1024 * 1024)),
           (unsigned)(DIRECT_MAP_BYTES / (1024 * 1024)));
}

uint64_t vmm_kernel_cr3(void) { return kernel_cr3_physical; }

static uint64_t device_arena_used;

uint64_t vmm_map_device(uint64_t physical, uint64_t bytes) {
    if (!physical || !bytes) return 0;
    lock_acquire(&tables_lock);
    uint64_t mapped = map_device_locked(physical, bytes, PAGE_UNCACHED);
    lock_release(&tables_lock);
    return mapped;
}

uint64_t vmm_map_device_write_combining(uint64_t physical, uint64_t bytes) {
    if (!physical || !bytes) return 0;
    uint64_t cache = vmm_write_combining_available() ? PAGE_WRITE_COMBINING : PAGE_UNCACHED;
    lock_acquire(&tables_lock);
    uint64_t mapped = map_device_locked(physical, bytes, cache);
    lock_release(&tables_lock);
    return mapped;
}

static uint64_t map_device_locked(uint64_t physical, uint64_t bytes, uint64_t cache) {
    uint64_t arena_used = device_arena_used;

    uint64_t page_offset = physical & 0xFFFULL;
    uint64_t first = physical - page_offset;
    uint64_t span = (bytes + page_offset + 0xFFFULL) & ~0xFFFULL;
    if (arena_used + span > DEVICE_ARENA_BYTES) return 0;

    uint64_t base = DEVICE_ARENA_BASE + arena_used;
    for (uint64_t offset = 0; offset < span; offset += 4096ULL) {
        if (vmm_map_page_in_locked(kernel_cr3_physical, base + offset, first + offset,
                                   PAGE_WRITE | PAGE_DEVICE | cache | PAGE_NX) != 0)
            return 0;
    }
    device_arena_used = arena_used + span;
    return base + page_offset;
}
uint64_t vmm_current_cr3(void) { return vmm_arch_read_root(); }

static uint64_t create_locked(struct vmm_space *space) {
    if (!space) return 0;
    uint64_t physical = (uint64_t)pmm_alloc_page();
    if (!physical) return 0;
    memset(space, 0, sizeof(*space));
    space->cr3 = physical;
    if (space_add(space) != 0) {
        pmm_free_page((void *)physical);
        return 0;
    }
    uint64_t *new_pml4 = page_table_pointer(physical);
    uint64_t *kernel_pml4 = page_table_pointer(kernel_cr3_physical);
    if (!new_pml4 || !kernel_pml4) {
        space_remove(physical);
        pmm_free_page((void *)physical);
        return 0;
    }
    memset(new_pml4, 0, 4096);
    for (uint64_t i = 256; i < 512; i++) new_pml4[i] = kernel_pml4[i];
    return physical;
}

void vmm_activate(uint64_t cr3_physical) {
    uint64_t physical = cr3_physical & ADDRESS_MASK;
    lock_acquire(&tables_lock);
    int known = address_space_registered(physical) != NULL;
    lock_release(&tables_lock);
    if (!known) {
        kprintf("VMM: activate rejected stale CR3=%p current=%p\n", (void *)physical,
                (void *)vmm_arch_read_root());
        panic("VMM: attempted to activate stale address space");
    }
    vmm_arch_write_root(physical);
}

int vmm_map_page_in(uint64_t cr3_physical, uint64_t virtual_address, uint64_t physical_address,
                    uint64_t flags) {
    lock_acquire(&tables_lock);
    int status = vmm_map_page_in_locked(cr3_physical, virtual_address, physical_address, flags);
    lock_release(&tables_lock);
    return status;
}

static int vmm_map_page_in_locked(uint64_t cr3_physical, uint64_t virtual_address,
                                  uint64_t physical_address, uint64_t flags) {
    uint64_t cr3 = cr3_physical & ADDRESS_MASK;
    struct vmm_space *space = address_space_registered(cr3);
    if (!space) return -1;
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
    if (pte_present(pt[i1])) return -2;
    pt[i1] = pte_page(physical_address & ADDRESS_MASK, flags | PAGE_PRESENT);
    account(space, 0, pt[i1]);
    if ((flags & PAGE_USER) && !(flags & (PAGE_NX | PAGE_DEVICE)))
        vmm_arch_sync_executable(physical_address);
    if (cr3 == vmm_arch_read_root() || kernel_mapping(virtual_address))
        vmm_arch_invalidate(virtual_address);
    return 0;
}

#define DEFERRED_FREE_MAX 64

struct flush_batch {
    unsigned depth;
    int pending;
    int kernel;
    uint64_t cr3;
    unsigned deferred_count;
    uint64_t deferred[DEFERRED_FREE_MAX];
};

static struct flush_batch batches[SMP_MAX_CPUS];

static struct flush_batch *my_batch(void) { return &batches[cpu_current()->index]; }

static void settle_batch(struct flush_batch *batch) {
    if (batch->kernel) {
        batch->kernel = 0;
        smp_flush_kernel_mappings();
    }
    if (batch->pending) {
        batch->pending = 0;
        uint64_t cr3 = batch->cr3;
        batch->cr3 = 0;
        smp_flush_address_space(cr3);
    }
    for (unsigned index = 0; index < batch->deferred_count; index++)
        pmm_free_page((void *)batch->deferred[index]);
    batch->deferred_count = 0;
}

void vmm_flush_batch_begin(void) { my_batch()->depth++; }

void vmm_flush_batch_end(void) {
    struct flush_batch *batch = my_batch();
    if (!batch->depth || --batch->depth) return;
    settle_batch(batch);
}

void vmm_free_page_after_flush(uint64_t physical) {
    if (!physical) return;
    struct flush_batch *batch = my_batch();
    if (!batch->depth) {
        pmm_free_page((void *)physical);
        return;
    }
    if (batch->deferred_count == DEFERRED_FREE_MAX) settle_batch(batch);
    batch->deferred[batch->deferred_count++] = physical;
}

static void invalidate_after_change(uint64_t cr3, uint64_t virtual_address);

static void flush_kernel_mapping(void) {
    struct flush_batch *batch = my_batch();
    if (!batch->depth) {
        smp_flush_kernel_mappings();
        return;
    }
    batch->kernel = 1;
}

static void flush_others(uint64_t cr3) {
    struct flush_batch *batch = my_batch();
    if (!batch->depth) {
        smp_flush_address_space(cr3);
        return;
    }
    if (batch->pending && batch->cr3 != cr3) smp_flush_address_space(batch->cr3);
    batch->cr3 = cr3;
    batch->pending = 1;
}

static void invalidate_after_change(uint64_t cr3, uint64_t virtual_address) {
    if (cr3 == vmm_arch_read_root() || kernel_mapping(virtual_address))
        vmm_arch_invalidate(virtual_address);
    if (kernel_mapping(virtual_address)) flush_kernel_mapping();
    else flush_others(cr3);
}

static int unmap_page_locked(uint64_t cr3_physical, uint64_t virtual_address) {
    uint64_t cr3 = cr3_physical & ADDRESS_MASK;
    struct vmm_space *space = address_space_registered(cr3);
    if (!space) return -1;
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
    if (!pt || !pte_present(pt[i1])) return -1;
    account(space, pt[i1], 0);
    pt[i1] = 0;
    invalidate_after_change(cr3, virtual_address);
    return 0;
}

static int table_is_empty(const uint64_t *table) {
    for (uint64_t index = 0; index < 512; index++)
        if (pte_present(table[index])) return 0;
    return 1;
}

static void prune_locked(uint64_t cr3_physical, uint64_t start, uint64_t end) {
    uint64_t cr3 = cr3_physical & ADDRESS_MASK;
    if (!address_space_registered(cr3) || start >= end) return;
    uint64_t *pml4 = page_table_pointer(cr3);
    if (!pml4) return;

    int freed = 0;
    vmm_flush_batch_begin();
    for (uint64_t address = start & ~0x1FFFFFULL; address < end; address += 0x200000ULL) {
        if (address >= USER_ADDRESS_LIMIT) break;
        uint16_t i4 = (address >> 39) & 0x1FF;
        uint16_t i3 = (address >> 30) & 0x1FF;
        uint16_t i2 = (address >> 21) & 0x1FF;
        if (i4 >= 256) break;

        uint64_t *pdpt = table_from_entry(pml4[i4]);
        if (!pdpt) continue;
        uint64_t *pd = table_from_entry(pdpt[i3]);
        if (!pd) continue;
        uint64_t *pt = table_from_entry(pd[i2]);
        if (!pt || !table_is_empty(pt)) continue;

        uint64_t page = pte_address(pd[i2]);
        pd[i2] = 0;
        vmm_free_page_after_flush(page);
        freed = 1;

        if (!table_is_empty(pd)) continue;
        page = pte_address(pdpt[i3]);
        pdpt[i3] = 0;
        vmm_free_page_after_flush(page);

        if (!table_is_empty(pdpt)) continue;
        page = pte_address(pml4[i4]);
        pml4[i4] = 0;
        vmm_free_page_after_flush(page);
    }

    if (freed) {
        if (cr3 == vmm_arch_read_root()) vmm_arch_write_root(cr3);
        flush_others(cr3);
    }
    vmm_flush_batch_end();
}

static int protect_page_locked(uint64_t cr3_physical, uint64_t virtual_address, uint64_t flags) {
    uint64_t cr3 = cr3_physical & ADDRESS_MASK;
    struct vmm_space *space = address_space_registered(cr3);
    if (!space) return -1;
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
    if (!pt || !pte_present(pt[i1])) return -1;
    uint64_t physical = pte_address(pt[i1]);
    uint64_t old_entry = pt[i1];
    pt[i1] = pte_page(physical, (flags & ~ADDRESS_MASK) | PAGE_PRESENT);
    account(space, old_entry, pt[i1]);
    if ((flags & PAGE_USER) && !(flags & (PAGE_NX | PAGE_DEVICE)))
        vmm_arch_sync_executable(physical);
    invalidate_after_change(cr3, virtual_address);
    return 0;
}

static int translate_locked(uint64_t cr3_physical, uint64_t virtual_address, uint64_t *physical_out,
                            uint64_t *flags_out) {
    uint64_t cr3 = cr3_physical & ADDRESS_MASK;
    if (!address_space_registered(cr3)) return -1;
    uint64_t *pml4 = page_table_pointer(cr3);
    if (!pml4) return -1;
    uint64_t e4 = pml4[(virtual_address >> 39) & 0x1FF];
    if (!pte_present(e4)) return -1;
    uint64_t f4 = pte_upper_flags(e4);
    uint64_t effective_user = f4 & PAGE_USER;
    uint64_t effective_write = f4 & PAGE_WRITE;
    uint64_t effective_nx = f4 & PAGE_NX;

    uint64_t *pdpt = table_from_entry(e4);
    if (!pdpt) return -1;
    uint64_t e3 = pdpt[(virtual_address >> 30) & 0x1FF];
    if (!pte_present(e3)) return -1;
    uint64_t f3 = pte_upper_flags(e3);
    effective_user &= f3;
    effective_write &= f3;
    effective_nx |= f3 & PAGE_NX;
    if (pte_huge(e3)) {
        if (physical_out)
            *physical_out = (pte_address(e3) & ~0x3FFFFFFFULL) | (virtual_address & 0x3FFFFFFFULL);
        if (flags_out) {
            uint64_t effective = f3;
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
    if (!pte_present(e2)) return -1;
    uint64_t f2 = pte_upper_flags(e2);
    effective_user &= f2;
    effective_write &= f2;
    effective_nx |= f2 & PAGE_NX;
    if (pte_huge(e2)) {
        if (physical_out)
            *physical_out = (pte_address(e2) & ~0x1FFFFFULL) | (virtual_address & 0x1FFFFFULL);
        if (flags_out) {
            uint64_t effective = f2;
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
    if (!pte_present(e1)) return -1;
    uint64_t f1 = pte_leaf_flags(e1);
    effective_user &= f1;
    effective_write &= f1;
    effective_nx |= f1 & PAGE_NX;
    if (physical_out) *physical_out = pte_address(e1) | (virtual_address & 0xFFF);
    if (flags_out) {
        uint64_t effective = f1;
        if (!effective_user) effective &= ~PAGE_USER;
        if (!effective_write) effective &= ~PAGE_WRITE;
        if (effective_nx) effective |= PAGE_NX;
        *flags_out = effective;
    }
    return 0;
}

int vmm_user_range_valid(uint64_t cr3_physical, uint64_t address, size_t length,
                         int write_required) {
    if (!length) return 1;
    if (address >= USER_ADDRESS_LIMIT || length > USER_ADDRESS_LIMIT - address) return 0;
    uint64_t first = address & ~0xFFFULL;
    uint64_t last = (address + length - 1) & ~0xFFFULL;
    for (uint64_t page = first;; page += 4096) {
        uint64_t flags;
        if (vmm_translate(cr3_physical, page, NULL, &flags) != 0) {
            if (cr3_physical != vmm_arch_read_root() ||
                (!process_commit_area(page) && !process_grow_user_stack(page)) ||
                vmm_translate(cr3_physical, page, NULL, &flags) != 0)
                return 0;
        }
        if (!(flags & PAGE_USER) || (write_required && !(flags & (PAGE_WRITE | PAGE_COW))))
            return 0;
        if (page == last) break;
    }
    return 1;
}

static int user_page_translate(uint64_t cr3_physical, uint64_t address, int write_required,
                               uint64_t *physical, uint64_t *flags) {
    if (vmm_translate(cr3_physical, address, physical, flags) != 0) {
        uint64_t page = address & ~0xFFFULL;
        if (cr3_physical != vmm_arch_read_root() ||
            (!process_commit_area(page) && !process_grow_user_stack(page)) ||
            vmm_translate(cr3_physical, address, physical, flags) != 0)
            return -1;
    }
    if (!(*flags & PAGE_USER) || (write_required && !(*flags & (PAGE_WRITE | PAGE_COW)))) return -1;
    return 0;
}

int vmm_copy_from_space(uint64_t cr3_physical, void *destination, uint64_t source_user,
                        size_t length) {
    if (source_user >= USER_ADDRESS_LIMIT || length > USER_ADDRESS_LIMIT - source_user) return -1;
    uint8_t *out = (uint8_t *)destination;
    while (length) {
        uint64_t physical;
        uint64_t flags;
        if (user_page_translate(cr3_physical, source_user, 0, &physical, &flags) != 0) return -1;
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

int vmm_copy_to_space(uint64_t cr3_physical, uint64_t destination_user, const void *source,
                      size_t length) {
    if (destination_user >= USER_ADDRESS_LIMIT || length > USER_ADDRESS_LIMIT - destination_user)
        return -1;
    const uint8_t *in = (const uint8_t *)source;
    while (length) {
        uint64_t physical;
        uint64_t flags;
        if (user_page_translate(cr3_physical, destination_user, 1, &physical, &flags) != 0)
            return -1;
        if (flags & PAGE_DEVICE) return -1;
        if (flags & PAGE_COW) {
            if (vmm_handle_cow_fault(cr3_physical, destination_user & ~0xFFFULL) != 0) return -1;
            if (vmm_translate(cr3_physical, destination_user, &physical, &flags) != 0) return -1;
        }
        if (!(flags & PAGE_WRITE)) return -1;
        size_t chunk = 4096 - (size_t)(destination_user & 0xFFF);
        if (chunk > length) chunk = length;
        if (!physical_direct_range_valid(physical, chunk)) return -1;
        memcpy((void *)(DIRECT_MAP_BASE + physical), in, chunk);
        if (!(flags & PAGE_NX)) vmm_arch_sync_executable(physical);
        in += chunk;
        destination_user += chunk;
        length -= chunk;
    }
    return 0;
}

void vmm_map_page(uint64_t virtual_address, uint64_t physical_address, uint16_t flags) {
    int status = vmm_map_page_in(kernel_cr3_physical, virtual_address, physical_address, flags);
    if (status != 0) panic("VMM: kernel map failed");
}

void vmm_unmap_page(uint64_t virtual_address) {
    vmm_unmap_page_in(kernel_cr3_physical, virtual_address);
}

static void destroy_user_table(uint64_t physical, int level, int64_t *seen);
static struct vmm_space *destroy_locked(uint64_t cr3_physical);

static uint64_t clone_user_table(uint64_t source_physical, int level, struct vmm_space *parent,
                                 struct vmm_space *child) {
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
        if (!pte_present(entry)) continue;
        if (pte_huge(entry)) {
            destroy_user_table(destination_physical, level, NULL);
            return 0;
        }
        uint64_t preserved_flags = pte_flags(entry);
        if (level == 1) {
            if (preserved_flags & PAGE_DEVICE) {
                destination[index] = entry;
            } else {
                uint64_t source_page = pte_address(entry);
                if (!pmm_page_is_allocated(source_page) ||
                    !physical_direct_range_valid(source_page, 4096)) {
                    destroy_user_table(destination_physical, level, NULL);
                    return 0;
                }
                if ((preserved_flags & PAGE_SHARED) && pmm_page_ref(source_page) == 0) {
                    destination[index] = entry;
                } else if (pmm_page_ref(source_page) == 0) {
                    uint64_t shared_flags = preserved_flags;
                    if (shared_flags & PAGE_WRITE) {
                        shared_flags = (shared_flags & ~PAGE_WRITE) | PAGE_COW;
                        source[index] = pte_page(source_page, shared_flags);
                        account(parent, entry, source[index]);
                    }
                    destination[index] = pte_page(source_page, shared_flags);
                } else {
                    uint64_t page_physical = (uint64_t)pmm_alloc_page();
                    if (!page_physical) {
                        destroy_user_table(destination_physical, level, NULL);
                        return 0;
                    }
                    memcpy((void *)(DIRECT_MAP_BASE + page_physical),
                           (void *)(DIRECT_MAP_BASE + source_page), 4096);
                    destination[index] = pte_retarget(entry, page_physical);
                }
            }
            account(child, 0, destination[index]);
        } else {
            uint64_t sub = clone_user_table(pte_address(entry), level - 1, parent, child);
            if (!sub) {
                destroy_user_table(destination_physical, level, NULL);
                return 0;
            }
            destination[index] = pte_retarget(entry, sub);
        }
    }
    return destination_physical;
}

static uint64_t clone_locked(uint64_t source_cr3, struct vmm_space *child) {
    uint64_t source_physical = source_cr3 & ADDRESS_MASK;
    struct vmm_space *parent = address_space_registered(source_physical);
    if (!parent) return 0;
    uint64_t destination_cr3 = create_locked(child);
    if (!destination_cr3) return 0;
    uint64_t *source = page_table_pointer(source_physical);
    uint64_t *destination = page_table_pointer(destination_cr3);
    if (!source || !destination) {
        (void)destroy_locked(destination_cr3);
        return 0;
    }
    for (uint64_t index = 0; index < 256; index++) {
        uint64_t entry = source[index];
        if (!pte_present(entry)) continue;
        if (pte_huge(entry)) {
            (void)destroy_locked(destination_cr3);
            return 0;
        }
        uint64_t sub = clone_user_table(pte_address(entry), 3, parent, child);
        if (!sub) {
            (void)destroy_locked(destination_cr3);
            return 0;
        }
        destination[index] = pte_retarget(entry, sub);
    }
    if (source_physical == vmm_arch_read_root()) vmm_arch_write_root(source_physical);
    smp_flush_address_space(source_physical);
    return destination_cr3;
}

static int cow_locked(uint64_t cr3_physical, uint64_t virtual_address) {
    uint64_t cr3 = cr3_physical & ADDRESS_MASK;
    struct vmm_space *space = address_space_registered(cr3);
    if (!space) return -1;
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
    uint64_t entry_flags = pte_flags(entry);
    if ((entry_flags & (PAGE_PRESENT | PAGE_USER | PAGE_WRITE | PAGE_COW)) ==
        (PAGE_PRESENT | PAGE_USER | PAGE_WRITE)) {
        if (cr3 == vmm_arch_read_root()) vmm_arch_invalidate(virtual_address);
        return 0;
    }
    if ((entry_flags & (PAGE_PRESENT | PAGE_COW | PAGE_USER)) !=
        (PAGE_PRESENT | PAGE_COW | PAGE_USER))
        return -1;

    uint64_t physical = pte_address(entry);
    uint64_t flags = (entry_flags & ~PAGE_COW & ~PAGE_FILEBACKED) | PAGE_WRITE;

    if (!(entry_flags & PAGE_FILEBACKED) && pmm_page_refcount(physical) <= 1) {
        pt[index] = pte_page(physical, flags);
    } else {
        if (!physical_direct_range_valid(physical, 4096)) return -1;
        uint64_t copy = (uint64_t)pmm_alloc_page();
        if (!copy || !physical_direct_range_valid(copy, 4096)) {
            if (copy) pmm_free_page((void *)copy);
            return -1;
        }
        memcpy((void *)(DIRECT_MAP_BASE + copy), (void *)(DIRECT_MAP_BASE + physical), 4096);
        pt[index] = pte_page(copy, flags);
        if (!(flags & (PAGE_NX | PAGE_DEVICE))) vmm_arch_sync_executable(copy);
        smp_flush_address_space(cr3);
        pmm_free_page((void *)physical);
    }
    account(space, entry, pt[index]);
    if (cr3 == vmm_arch_read_root()) vmm_arch_invalidate(virtual_address);
    return 0;
}

static void destroy_user_table(uint64_t physical, int level, int64_t *seen) {
    physical &= ADDRESS_MASK;
    uint64_t *table = page_table_pointer(physical);
    if (!table) {
        KDEBUG("VMM: skipped stale user page table %p\n", (void *)physical);
        return;
    }
    for (uint64_t index = 0; index < 512; index++) {
        uint64_t entry = table[index];
        if (!pte_present(entry)) continue;
        if (pte_huge(entry)) {
            KDEBUG("VMM: skipped unexpected user huge page\n");
            continue;
        }
        uint64_t child = pte_address(entry);
        if (level == 1) {
            int kind = usage_kind(entry);
            if (seen && kind >= 0) seen[kind]++;
            if (!(pte_flags(entry) & PAGE_DEVICE) && pmm_page_is_allocated(child))
                pmm_free_page((void *)child);
        } else {
            destroy_user_table(child, level - 1, seen);
        }
        table[index] = 0;
    }
    if (pmm_page_is_allocated(physical)) pmm_free_page((void *)physical);
}

static struct vmm_space *destroy_locked(uint64_t cr3_physical) {
    uint64_t physical = cr3_physical & ADDRESS_MASK;
    if (physical == kernel_cr3_physical || !physical) return NULL;
    struct vmm_space *space = address_space_registered(physical);
    if (!space) {
        KDEBUG("VMM: duplicate/stale address-space destroy %p ignored\n", (void *)physical);
        return NULL;
    }
    if (physical == vmm_arch_read_root()) {
        kprintf("VMM: refused to destroy active CR3=%p\n", (void *)physical);
        return NULL;
    }
    uint64_t *pml4 = page_table_pointer(physical);
    if (!pml4) {
        space_remove(physical);
        return space;
    }
    int64_t seen[VMM_USAGE_KINDS] = {0};
    for (uint64_t index = 0; index < 256; index++) {
        uint64_t entry = pml4[index];
        if (pte_present(entry)) destroy_user_table(pte_address(entry), 3, seen);
        pml4[index] = 0;
    }
    static unsigned drift_reports;
    for (unsigned kind = 0; kind < VMM_USAGE_KINDS; kind++) {
        if (seen[kind] == space->pages[kind] || drift_reports >= 8U) continue;
        drift_reports++;
        kprintf("VMM: usage drift in %p kind %u: counted %d, found %d\n", (void *)physical, kind,
                (int)space->pages[kind], (int)seen[kind]);
    }
    space_remove(physical);
    if (pmm_page_is_allocated(physical)) pmm_free_page((void *)physical);
    return space;
}

int vmm_unmap_page_in(uint64_t cr3_physical, uint64_t virtual_address) {
    lock_acquire(&tables_lock);
    int status = unmap_page_locked(cr3_physical, virtual_address);
    lock_release(&tables_lock);
    return status;
}

void vmm_prune_empty_tables(uint64_t cr3_physical, uint64_t start, uint64_t end) {
    lock_acquire(&tables_lock);
    prune_locked(cr3_physical, start, end);
    lock_release(&tables_lock);
}

int vmm_protect_page_in(uint64_t cr3_physical, uint64_t virtual_address, uint64_t flags) {
    lock_acquire(&tables_lock);
    int status = protect_page_locked(cr3_physical, virtual_address, flags);
    lock_release(&tables_lock);
    return status;
}

int vmm_translate(uint64_t cr3_physical, uint64_t virtual_address, uint64_t *physical_out,
                  uint64_t *flags_out) {
    lock_acquire(&tables_lock);
    int status = translate_locked(cr3_physical, virtual_address, physical_out, flags_out);
    lock_release(&tables_lock);
    return status;
}

uint64_t vmm_clone_address_space(uint64_t source_cr3) {
    struct vmm_space *child = (struct vmm_space *)kmalloc(sizeof(*child));
    if (!child) return 0;
    lock_acquire(&tables_lock);
    uint64_t cloned = clone_locked(source_cr3, child);
    lock_release(&tables_lock);
    if (!cloned) kfree(child);
    return cloned;
}

int vmm_handle_cow_fault(uint64_t cr3_physical, uint64_t virtual_address) {
    lock_acquire(&tables_lock);
    int status = cow_locked(cr3_physical, virtual_address);
    lock_release(&tables_lock);
    return status;
}

void vmm_destroy_address_space(uint64_t cr3_physical) {
    lock_acquire(&tables_lock);
    struct vmm_space *space = destroy_locked(cr3_physical);
    lock_release(&tables_lock);
    kfree(space);
}

int vmm_usage(uint64_t cr3_physical, uint64_t usage[VMM_USAGE_KINDS]) {
    lock_acquire(&tables_lock);
    struct vmm_space *space = address_space_registered(cr3_physical);
    for (unsigned kind = 0; kind < VMM_USAGE_KINDS; kind++)
        usage[kind] = space && space->pages[kind] > 0 ? (uint64_t)space->pages[kind] : 0;
    lock_release(&tables_lock);
    return space ? 0 : -1;
}

uint64_t vmm_count_user_pages(uint64_t cr3_physical) {
    uint64_t usage[VMM_USAGE_KINDS];
    if (vmm_usage(cr3_physical, usage) != 0) return 0;
    return usage[VMM_USAGE_ANON] + usage[VMM_USAGE_FILE] + usage[VMM_USAGE_SHMEM];
}

uint64_t vmm_create_address_space(void) {
    struct vmm_space *space = (struct vmm_space *)kmalloc(sizeof(*space));
    if (!space) return 0;
    lock_acquire(&tables_lock);
    uint64_t created = create_locked(space);
    lock_release(&tables_lock);
    if (!created) kfree(space);
    return created;
}
