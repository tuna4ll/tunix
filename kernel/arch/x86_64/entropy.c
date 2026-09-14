#include <stddef.h>
#include <stdint.h>

#include "../../include/cpu.h"
#include "../../include/io.h"
#include "../../include/random.h"

static int cpu_has_rdrand(void) {
    uint32_t a, b, c, d;
    cpu_cpuid(1, 0, &a, &b, &c, &d);
    return (c & (1U << 30)) != 0;
}

static int cpu_has_rdseed(void) {
    uint32_t a, b, c, d;
    cpu_cpuid(0, 0, &a, &b, &c, &d);
    if (a < 7U) return 0;
    cpu_cpuid(7, 0, &a, &b, &c, &d);
    return (b & (1U << 18)) != 0;
}

static int get_rdrand(uint64_t *value) {
    uint64_t result;
    unsigned char ok;
    __asm__ volatile("rdrand %0; setc %1" : "=r"(result), "=m"(ok) : : "cc");
    if (ok) *value = result;
    return ok != 0;
}

static int get_rdseed(uint64_t *value) {
    uint64_t result;
    unsigned char ok;
    __asm__ volatile("rdseed %0; setc %1" : "=r"(result), "=m"(ok) : : "cc");
    if (ok) *value = result;
    return ok != 0;
}

size_t arch_entropy_collect(uint64_t *values, size_t room) {
    size_t count = 0;
    uint32_t a, b, c, d;
    if (room < 2U) return 0;

    cpu_cpuid(0, 0, &a, &b, &c, &d);
    values[count++] = ((uint64_t)a << 32) | b;
    values[count++] = ((uint64_t)c << 32) | d;

    if (cpu_has_rdseed()) {
        for (unsigned i = 0; i < 8 && count < room; i++) {
            uint64_t value;
            for (unsigned retry = 0; retry < 32U; retry++) {
                if (get_rdseed(&value)) { values[count++] = value; break; }
                cpu_relax();
            }
        }
    }
    if (cpu_has_rdrand()) {
        for (unsigned i = 0; i < 8 && count < room; i++) {
            uint64_t value;
            for (unsigned retry = 0; retry < 16U; retry++) {
                if (get_rdrand(&value)) { values[count++] = value; break; }
                cpu_relax();
            }
        }
    }
    return count;
}

uint64_t arch_entropy_noise(void) {
    return inb(0x61U);
}
