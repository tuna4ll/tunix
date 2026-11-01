#if defined(TUNIX_KASAN)

#include <stddef.h>
#include <stdint.h>
#include <tunix/heap.h>
#include <tunix/kasan.h>
#include <tunix/kprintf.h>

static volatile uint8_t sink;

__attribute__((noinline)) static uint8_t touch(const volatile uint8_t *pointer) { return *pointer; }

__attribute__((noinline)) static void store(volatile uint8_t *pointer, uint8_t value) {
    *pointer = value;
}

void kasan_selftest(void) {
    uint32_t before, after;
    unsigned passed = 0;

    before = kasan_report_count();
    volatile uint8_t *freed = kmalloc(64);
    if (freed) {
        kfree((void *)freed);
        sink = touch(freed + 8);
    }
    after = kasan_report_count();
    if (after == before + 1U) passed++;
    else kprintf("KASAN-SELFTEST: use-after-free was not reported\n");

    before = after;
    volatile uint8_t *overflow = kmalloc(64);
    if (overflow) {
        store(overflow + 72, 0xAA);
        kfree((void *)overflow);
    }
    after = kasan_report_count();
    if (after == before + 1U) passed++;
    else kprintf("KASAN-SELFTEST: out-of-bounds was not reported\n");

    before = after;
    volatile uint8_t *twice = kmalloc(64);
    if (twice) {
        kfree((void *)twice);
        kfree((void *)twice);
    }
    after = kasan_report_count();
    if (after == before + 1U) passed++;
    else kprintf("KASAN-SELFTEST: double-free was not reported\n");

    kprintf("KASAN-SELFTEST: %u/3 reported, %u total\n", passed, kasan_report_count());
}

#endif
