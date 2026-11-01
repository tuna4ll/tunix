#include <stddef.h>
#include <stdint.h>
#include <tunix/cpu.h>
#include <tunix/kallsyms.h>
#include <tunix/random.h>
#include <tunix/kprintf.h>

uint64_t __stack_chk_guard = 0x5D1C7A3E9B20F600ULL;

void __stack_chk_fail(void) __attribute__((noreturn));
__attribute__((no_stack_protector)) void __stack_chk_fail(void) {
    static const char prefix[] = "stack-protector: Kernel stack is corrupted in: ";
    static char message[sizeof(prefix) + 160];
    size_t used = 0;
    for (; prefix[used]; used++) message[used] = prefix[used];
    kallsyms_format((uint64_t)__builtin_return_address(0), message + used, sizeof(message) - used);
    panic(message);
}

__attribute__((no_stack_protector)) void stack_guard_init(void) {
    uint64_t values[18];
    size_t count = arch_entropy_collect(values, sizeof(values) / sizeof(values[0]));
    uint64_t mixed = __stack_chk_guard ^ cpu_counter();
    for (size_t index = 0; index < count; index++) {
        mixed = (mixed ^ values[index]) * 0x9E3779B97F4A7C15ULL;
        mixed ^= mixed >> 29;
    }
    mixed &= ~0xFFULL;
    if (mixed) __stack_chk_guard = mixed;
}
