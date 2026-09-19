#include <stddef.h>
#include <stdint.h>

#include "heap.h"
#include "kstring.h"
#include "module.h"
#include "time.h"

extern void kprintf(const char *fmt, ...);

static int number;
static int flag;
static unsigned count = 1;
static int fail;
static int crash;
static char *text = "none";

MODULE_PARAMETER(number, MODULE_PARAM_INT);
MODULE_PARAMETER(flag, MODULE_PARAM_BOOL);
MODULE_PARAMETER(count, MODULE_PARAM_UINT);
MODULE_PARAMETER(fail, MODULE_PARAM_INT);
MODULE_PARAMETER(crash, MODULE_PARAM_INT);
MODULE_PARAMETER(text, MODULE_PARAM_STRING);

static const char *const shapes[] = { "zero", "one", "two", "three" };

static uint64_t scale(unsigned kind, uint64_t value) {
    switch (kind) {
    case 0: return value + 1ULL;
    case 1: return value * 3ULL;
    case 2: return value << 4;
    case 3: return value - 7ULL;
    default: return 0;
    }
}

uint64_t tunix_probe_sum(unsigned count) {
    uint64_t total = 0;
    for (unsigned index = 0; index < count; index++)
        total += scale(index & 3U, index);
    return total;
}

const char *tunix_probe_shape(unsigned index) {
    return shapes[index & 3U];
}

MODULE_EXPORT(tunix_probe_sum);
MODULE_EXPORT(tunix_probe_shape);

static int probe_init(void) {
    if (crash) {
        kprintf("TUNIXPROBE about to fault on purpose\n");
        volatile uint64_t *nowhere = (volatile uint64_t *)0;
        *nowhere = 1;
    }
    if (fail) {
        kprintf("TUNIXPROBE refusing to start, fail=%d\n", fail);
        return fail;
    }
    char *buffer = kmalloc(32);
    if (!buffer) return -12;
    memcpy(buffer, "heap", 5);
    uint64_t now = time_uptime_ns();
    kprintf("TUNIXPROBE loaded number=%d flag=%d text=%s count=%u sum=%u shape=%s %s=%u\n",
            number, flag, text, count, (unsigned)tunix_probe_sum(8),
            tunix_probe_shape(2), buffer, (unsigned)(now != 0ULL));
    kfree(buffer);
    return 0;
}

static void probe_exit(void) {
    kprintf("TUNIXPROBE unloaded number=%d\n", number);
}

MODULE_MAIN(probe_init, probe_exit);
MODULE_LICENSE("MIT");
MODULE_DESCRIPTION("Relocation and parameter check for the module loader");
MODULE_AUTHOR("Tunix");
