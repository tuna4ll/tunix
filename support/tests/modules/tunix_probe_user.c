#include <stdint.h>

#include "module.h"

extern void kprintf(const char *fmt, ...);
extern uint64_t tunix_probe_sum(unsigned count);
extern const char *tunix_probe_shape(unsigned index);

static int user_init(void) {
    kprintf("TUNIXPROBEUSER loaded sum=%u shape=%s\n",
            (unsigned)tunix_probe_sum(16), tunix_probe_shape(3));
    return 0;
}

static void user_exit(void) {
    kprintf("TUNIXPROBEUSER unloaded\n");
}

MODULE_MAIN(user_init, user_exit);
MODULE_LICENSE("MIT");
MODULE_DESCRIPTION("Checks that one module can use another module's symbols");
