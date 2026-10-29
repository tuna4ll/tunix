#include <stddef.h>
#include <stdint.h>
#include <tunix/kprintf.h>
#include <tunix/mutex.h>
#include <tunix/pmm.h>
#include <tunix/process.h>
#include <tunix/reclaim.h>
#include <tunix/swap.h>
#include <tunix/time.h>
#include <tunix/vfs.h>
#include <tunix/vmm.h>

#define RECLAIM_MEMORIES    256U
#define RECLAIM_SCAN_BUDGET 8192ULL
#define RECLAIM_PASSES      3U
#define KSWAPD_POLL_NS      (100ULL * 1000ULL * 1000ULL)
#define RECLAIM_MIN_FLOOR   256ULL
#define RECLAIM_MIN_CEILING 16384ULL

static const char kswapd_channel;
static volatile int kswapd_requested;
static struct process *kswapd_task;
static uint64_t scan_rotation;
static uint64_t pages_reclaimed;
static uint64_t pages_swapped_out;
static uint64_t pages_dropped;

uint64_t reclaim_min_pages(void) {
    uint64_t pages = pmm_usable_page_count() / 128ULL;
    if (pages < RECLAIM_MIN_FLOOR) pages = RECLAIM_MIN_FLOOR;
    if (pages > RECLAIM_MIN_CEILING) pages = RECLAIM_MIN_CEILING;
    return pages;
}

uint64_t reclaim_low_pages(void) { return reclaim_min_pages() * 2ULL; }

uint64_t reclaim_high_pages(void) { return reclaim_min_pages() * 3ULL; }

struct scan_context {
    struct process_memory *memory;
    int may_swap;
};

static int decide(void *context, uint64_t address, uint64_t flags) {
    struct scan_context *scan = (struct scan_context *)context;
    uint32_t kind = process_memory_area_kind(scan->memory, address);
    if (kind & (VM_DEVICE | VM_MEMFD)) return VMM_RECLAIM_KEEP;
    if (flags & PAGE_FILEBACKED) {
        if (!(kind & VM_FILE_PAGES)) return VMM_RECLAIM_KEEP;
        if ((flags & PAGE_SHARED) && (kind & VM_SHARED_WRITE)) return VMM_RECLAIM_KEEP;
        return VMM_RECLAIM_DROP;
    }
    return scan->may_swap ? VMM_RECLAIM_SWAP : VMM_RECLAIM_KEEP;
}

static uint64_t scan_memories(uint64_t wanted, int may_swap) {
    struct process_memory *memories[RECLAIM_MEMORIES];
    unsigned count = process_collect_memories(memories, RECLAIM_MEMORIES, 0);
    if (!count) return 0;
    uint64_t released = 0;
    uint64_t start = scan_rotation++ % count;
    for (unsigned pass = 0; pass < RECLAIM_PASSES && released < wanted; pass++) {
        for (unsigned step = 0; step < count && released < wanted; step++) {
            struct process_memory *memory = memories[(start + step) % count];
            if (!mutex_trylock(&memory->lock)) continue;
            struct scan_context context = {memory, may_swap};
            struct vmm_reclaim scan = {
                .cursor = memory->reclaim_cursor,
                .budget = RECLAIM_SCAN_BUDGET,
                .decide = decide,
                .context = &context,
            };
            if (vmm_reclaim_scan(memory->cr3, &scan) == 0) {
                memory->reclaim_cursor = scan.cursor;
                released += scan.dropped + scan.swapped;
                __atomic_add_fetch(&pages_dropped, scan.dropped, __ATOMIC_RELAXED);
                __atomic_add_fetch(&pages_swapped_out, scan.swapped, __ATOMIC_RELAXED);
            }
            mutex_unlock(&memory->lock);
        }
    }
    for (unsigned index = 0; index < count; index++) process_memory_put(memories[index]);
    return released;
}

uint64_t reclaim_pages(uint64_t wanted) {
    if (!wanted) return 0;
    uint64_t before = pmm_free_page_count();
    int may_swap = swap_free_pages() > 0;
    if (may_swap) (void)swap_reserve_entries(256U);
    (void)swap_writeback(64U);
    uint64_t freed = vfs_reclaim_pages(wanted);
    if (freed < wanted) {
        (void)scan_memories(wanted - freed, may_swap);
        (void)swap_writeback((unsigned)(wanted > 4096ULL ? 4096ULL : wanted) * 2U);
        if (pmm_free_page_count() <= before + freed) (void)vfs_reclaim_pages(wanted - freed);
    }
    uint64_t after = pmm_free_page_count();
    uint64_t gained = after > before ? after - before : 0;
    __atomic_add_fetch(&pages_reclaimed, gained, __ATOMIC_RELAXED);
    return gained;
}

void reclaim_wake(void) {
    if (!kswapd_task || __atomic_exchange_n(&kswapd_requested, 1, __ATOMIC_ACQ_REL)) return;
    process_wake_all(&kswapd_channel);
}

void reclaim_note_allocation(void) {
    if (pmm_free_page_count() < reclaim_low_pages()) reclaim_wake();
}

static void kswapd(void *unused) {
    (void)unused;
    for (;;) {
        process_prepare_wait(&kswapd_channel, time_uptime_ns() + KSWAPD_POLL_NS);
        if (!__atomic_load_n(&kswapd_requested, __ATOMIC_ACQUIRE) &&
            pmm_free_page_count() >= reclaim_low_pages())
            process_wait();
        process_finish_wait();
        __atomic_store_n(&kswapd_requested, 0, __ATOMIC_RELEASE);
        unsigned idle_rounds = 0;
        while (pmm_free_page_count() < reclaim_high_pages() && idle_rounds < 4U) {
            uint64_t wanted = reclaim_high_pages() - pmm_free_page_count();
            if (wanted > 2048ULL) wanted = 2048ULL;
            if (reclaim_pages(wanted)) idle_rounds = 0;
            else idle_rounds++;
        }
        if (swap_cached_pages()) (void)swap_writeback(256U);
        else swap_discard_released();
    }
}

void reclaim_start(void) {
    kswapd_task = process_create_kthread("kswapd0", kswapd, NULL);
    if (!kswapd_task) kprintf("RECLAIM: cannot start kswapd\n");
}

void reclaim_statistics(uint64_t *reclaimed, uint64_t *swapped_out, uint64_t *dropped) {
    *reclaimed = __atomic_load_n(&pages_reclaimed, __ATOMIC_RELAXED);
    *swapped_out = __atomic_load_n(&pages_swapped_out, __ATOMIC_RELAXED);
    *dropped = __atomic_load_n(&pages_dropped, __ATOMIC_RELAXED);
}
