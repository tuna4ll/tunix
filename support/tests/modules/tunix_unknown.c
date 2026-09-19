#include <stdint.h>

#include "module.h"

extern uint64_t tunix_symbol_that_does_not_exist(uint64_t value);

static int unknown_init(void) {
    return (int)tunix_symbol_that_does_not_exist(1);
}

static void unknown_exit(void) {
}

MODULE_MAIN(unknown_init, unknown_exit);
MODULE_LICENSE("MIT");
MODULE_DESCRIPTION("A module the loader has to refuse: it needs a symbol nobody exports");
