#include <stddef.h>
#include <stdint.h>

#include "../include/heap.h"
#include "../include/kstring.h"
#include "../include/module.h"
#include "../include/time.h"

extern void kprintf(const char *fmt, ...);

#define SELFTEST_ANSWER 0x5AD2C0DEBEEF0007ULL

static const char *const words[] = { "tunix", "module", "self", "test" };
static uint64_t table[8] = { 3, 5, 7, 11, 13, 17, 19, 23 };
static uint64_t mixed;

static uint64_t fold(unsigned kind, uint64_t value) {
    switch (kind) {
    case 0: return value * 0x9E3779B97F4A7C15ULL;
    case 1: return (value << 13) ^ (value >> 7);
    case 2: return value + 0x1000193ULL;
    case 3: return value ^ (value >> 33);
    default: return value;
    }
}

uint64_t tunix_selftest_run(uint64_t seed) {
    uint64_t value = seed;
    for (unsigned index = 0; index < 8U; index++)
        value = fold(index & 3U, value + table[index]);
    for (unsigned index = 0; index < 4U; index++)
        value = fold(index & 3U, value + strlen(words[index]) + (uint64_t)words[index][0]);
    return value ^ mixed;
}

const uint64_t tunix_selftest_answer = SELFTEST_ANSWER;

MODULE_EXPORT(tunix_selftest_run);
MODULE_EXPORT(tunix_selftest_answer);

static int selftest_init(void) {
    char *scratch = kmalloc(64);
    if (!scratch) return -12;
    memcpy(scratch, "relocated", 10);
    mixed = 0;
    uint64_t computed = tunix_selftest_run(7);
    mixed = computed ^ SELFTEST_ANSWER;
    int ok = tunix_selftest_run(7) == SELFTEST_ANSWER && strcmp(scratch, "relocated") == 0 &&
             time_uptime_ns() != 0ULL;
    kfree(scratch);
    if (!ok) {
        kprintf("SELFTEST: the module did not compute what it should\n");
        return -22;
    }
    return 0;
}

static void selftest_exit(void) {
    mixed = 0;
}

MODULE_MAIN(selftest_init, selftest_exit);
MODULE_LICENSE("MIT");
MODULE_DESCRIPTION("Checks the module loader on the machine it is running on");
MODULE_AUTHOR("Tunix");
