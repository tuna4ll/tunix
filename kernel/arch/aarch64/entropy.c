#include <stddef.h>
#include <stdint.h>

#include "../../include/cpu.h"
#include "../../include/random.h"

static int cpu_has_rndr(void) {
    uint64_t isar0;
    __asm__ volatile("mrs %0, id_aa64isar0_el1" : "=r"(isar0));
    return ((isar0 >> 60) & 0xFU) != 0;
}

static int get_rndr(uint64_t *value) {
    uint64_t result, ok;
    __asm__ volatile("mrs %0, s3_3_c2_c4_0; cset %1, ne" : "=r"(result), "=r"(ok) : : "cc");
    if (ok) *value = result;
    return ok != 0;
}

static int get_rndrrs(uint64_t *value) {
    uint64_t result, ok;
    __asm__ volatile("mrs %0, s3_3_c2_c4_1; cset %1, ne" : "=r"(result), "=r"(ok) : : "cc");
    if (ok) *value = result;
    return ok != 0;
}

size_t arch_entropy_collect(uint64_t *values, size_t room) {
    size_t count = 0;
    if (room < 2U) return 0;

    uint64_t midr;
    __asm__ volatile("mrs %0, midr_el1" : "=r"(midr));
    values[count++] = midr;
    values[count++] = cpu_counter();

    if (!cpu_has_rndr()) return count;
    for (unsigned i = 0; i < 8 && count < room; i++) {
        uint64_t value;
        for (unsigned retry = 0; retry < 32U; retry++) {
            if (get_rndrrs(&value)) { values[count++] = value; break; }
            cpu_relax();
        }
    }
    for (unsigned i = 0; i < 8 && count < room; i++) {
        uint64_t value;
        for (unsigned retry = 0; retry < 16U; retry++) {
            if (get_rndr(&value)) { values[count++] = value; break; }
            cpu_relax();
        }
    }
    return count;
}

uint64_t arch_entropy_noise(void) {
    return cpu_counter();
}
