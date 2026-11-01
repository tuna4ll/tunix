#if defined(TUNIX_KASAN)

#include <stddef.h>
#include <stdint.h>
#include <tunix/kallsyms.h>
#include <tunix/kasan.h>
#include <tunix/kprintf.h>
#include <tunix/kstring.h>
#include <tunix/oops.h>
#include <tunix/percpu.h>
#include <tunix/pmm.h>
#include <tunix/process.h>
#include <tunix/vmm.h>

static uint64_t covered;
static uint64_t shadow_mapped;
static uint32_t reports;
static int reporting;

static inline uint8_t *shadow_of(uint64_t address) {
    return (uint8_t *)(KASAN_SHADOW_BASE + ((address - HEAP_VIRTUAL_BASE) >> 3));
}

static inline int in_heap(uint64_t address) {
    return address >= HEAP_VIRTUAL_BASE &&
        address < HEAP_VIRTUAL_BASE + __atomic_load_n(&covered, __ATOMIC_ACQUIRE);
}

int kasan_cover(uint64_t start, uint64_t length) {
    if (start != HEAP_VIRTUAL_BASE + covered) return -1;
    uint64_t end = covered + length;
    uint64_t wanted = ((end >> 3) + PMM_PAGE_SIZE - 1U) & ~(PMM_PAGE_SIZE - 1U);
    for (; shadow_mapped < wanted; shadow_mapped += PMM_PAGE_SIZE) {
        void *physical = pmm_alloc_page();
        if (!physical) return -1;
        memset(vmm_phys_to_virt((uint64_t)physical), 0, PMM_PAGE_SIZE);
        if (vmm_map_page_in(vmm_kernel_cr3(), KASAN_SHADOW_BASE + shadow_mapped, (uint64_t)physical,
                            PAGE_PRESENT | PAGE_WRITE | PAGE_NX) != 0) {
            pmm_free_page(physical);
            return -1;
        }
    }
    __atomic_store_n(&covered, end, __ATOMIC_RELEASE);
    return 0;
}

void kasan_poison(uint64_t start, uint64_t length, uint8_t value) {
    if (!length || !in_heap(start)) return;
    uint8_t *shadow = shadow_of(start);
    uint64_t full = length >> 3;
    for (uint64_t index = 0; index < full; index++) shadow[index] = value;
    uint64_t tail = length & 7U;
    if (tail) shadow[full] = value == 0 ? (uint8_t)tail : value;
}

void kasan_unpoison(uint64_t start, uint64_t length) {
    if (!in_heap(start)) return;
    uint64_t aligned = length & ~7ULL;
    if (aligned) kasan_poison(start, aligned, 0);
    uint64_t tail = length & 7U;
    if (tail) *shadow_of(start + aligned) = (uint8_t)tail;
}

static const char *shadow_name(uint8_t value) {
    switch (value) {
    case KASAN_FREE_SPACE: return "heap-free";
    case KASAN_FREED:      return "use-after-free";
    case KASAN_REDZONE:    return "heap-out-of-bounds";
    default:               return "heap-out-of-bounds";
    }
}

static void report(uint64_t address, size_t size, int write, uint64_t caller, uint8_t shadow) {
    if (__atomic_exchange_n(&reporting, 1, __ATOMIC_ACQ_REL)) return;
    __atomic_add_fetch(&reports, 1, __ATOMIC_RELAXED);
    oops_begin();
    kprintf("==================================================================\n");
    kprintf("BUG: KASAN: %s in ", shadow_name(shadow));
    oops_print_symbol(caller);
    kprintf("\n%s of size %u at addr ", write ? "Write" : "Read", (unsigned)size);
    oops_print_hex(address);
    kprintf(" by task ");
    struct process *task = process_current();
    kprintf("%s/%d\n", task ? task->name : "swapper", task ? (int)task->pid : 0);
    oops_print_task();
    oops_print_trace((uint64_t)__builtin_frame_address(0), (uint64_t)__builtin_frame_address(0));
    kprintf("==================================================================\n");
    oops_end();
    __atomic_store_n(&reporting, 0, __ATOMIC_RELEASE);
}

void kasan_report_free(const void *pointer, uint64_t caller, int double_free) {
    report((uint64_t)pointer, 0, 1, caller, double_free ? KASAN_FREED : KASAN_REDZONE);
    if (__atomic_load_n(&reports, __ATOMIC_RELAXED) == 1)
        kprintf("BUG: KASAN: double-free or invalid-free\n");
}

uint32_t kasan_report_count(void) { return __atomic_load_n(&reports, __ATOMIC_RELAXED); }

void kasan_check_range(uint64_t address, size_t size, int write, uint64_t caller) {
    if (!size || !in_heap(address)) return;
    uint64_t at = address;
    uint64_t end = address + size;
    while (at < end) {
        uint64_t granule = at & ~7ULL;
        uint64_t granule_end = granule + 8U;
        uint64_t touched = (end < granule_end ? end : granule_end) - 1U;
        uint8_t shadow = *shadow_of(granule);
        uint8_t last_offset = (uint8_t)(touched & 7U);
        if (shadow != 0 && ((int8_t)shadow < 0 || last_offset >= shadow)) {
            report(address, size, write, caller, shadow);
            return;
        }
        at = granule_end;
    }
}

#define LOAD(bytes) \
    void __asan_load##bytes(uint64_t address); \
    void __asan_load##bytes(uint64_t address) { \
        kasan_check_range(address, bytes, 0, (uint64_t)__builtin_return_address(0)); \
    } \
    void __asan_load##bytes##_noabort(uint64_t address); \
    void __asan_load##bytes##_noabort(uint64_t address) { \
        kasan_check_range(address, bytes, 0, (uint64_t)__builtin_return_address(0)); \
    }
#define STORE(bytes) \
    void __asan_store##bytes(uint64_t address); \
    void __asan_store##bytes(uint64_t address) { \
        kasan_check_range(address, bytes, 1, (uint64_t)__builtin_return_address(0)); \
    } \
    void __asan_store##bytes##_noabort(uint64_t address); \
    void __asan_store##bytes##_noabort(uint64_t address) { \
        kasan_check_range(address, bytes, 1, (uint64_t)__builtin_return_address(0)); \
    }

LOAD(1)
LOAD(2)
LOAD(4)
LOAD(8)
LOAD(16)
STORE(1)
STORE(2)
STORE(4)
STORE(8)
STORE(16)

void __asan_loadN(uint64_t address, size_t size);
void __asan_loadN(uint64_t address, size_t size) {
    kasan_check_range(address, size, 0, (uint64_t)__builtin_return_address(0));
}
void __asan_loadN_noabort(uint64_t address, size_t size);
void __asan_loadN_noabort(uint64_t address, size_t size) {
    kasan_check_range(address, size, 0, (uint64_t)__builtin_return_address(0));
}
void __asan_storeN(uint64_t address, size_t size);
void __asan_storeN(uint64_t address, size_t size) {
    kasan_check_range(address, size, 1, (uint64_t)__builtin_return_address(0));
}
void __asan_storeN_noabort(uint64_t address, size_t size);
void __asan_storeN_noabort(uint64_t address, size_t size) {
    kasan_check_range(address, size, 1, (uint64_t)__builtin_return_address(0));
}

void __asan_handle_no_return(void);
void __asan_handle_no_return(void) {}
void __asan_before_dynamic_init(uint64_t module);
void __asan_before_dynamic_init(uint64_t module) { (void)module; }
void __asan_after_dynamic_init(void);
void __asan_after_dynamic_init(void) {}
void __asan_register_globals(void *globals, size_t count);
void __asan_register_globals(void *globals, size_t count) {
    (void)globals;
    (void)count;
}
void __asan_unregister_globals(void *globals, size_t count);
void __asan_unregister_globals(void *globals, size_t count) {
    (void)globals;
    (void)count;
}

#endif
