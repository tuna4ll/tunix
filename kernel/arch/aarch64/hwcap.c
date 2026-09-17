#include <stddef.h>
#include <stdint.h>

#include "../../include/hwcap.h"

static const char *const names[] = {
    "fp", "asimd", "evtstrm", "aes", "pmull", "sha1", "sha2", "crc32",
    "atomics", "fphp", "asimdhp", "cpuid", "asimdrdm", "jscvt", "fcma", "lrcpi",
    "dcpop", "sha3", "sm3", "sm4", "asimddp", "sha512", "sve", "asimdfhm",
    "dit", "uscat", "ilrcpi", "flagm", "ssbs", "sb", "paca", "pacg",
};

static uint64_t shared_hwcap = ~0ULL;

static unsigned field(uint64_t value, unsigned shift) {
    return (unsigned)((value >> shift) & 0xFU);
}

static uint64_t bit(unsigned index) {
    return 1ULL << index;
}

static uint64_t current_hwcap(void) {
    uint64_t pfr0, isar0, isar1;
    __asm__ volatile("mrs %0, id_aa64pfr0_el1" : "=r"(pfr0));
    __asm__ volatile("mrs %0, id_aa64isar0_el1" : "=r"(isar0));
    __asm__ volatile("mrs %0, id_aa64isar1_el1" : "=r"(isar1));

    uint64_t hwcap = 0;
    unsigned fp = field(pfr0, 16), simd = field(pfr0, 20);
    if (fp != 0xFU) hwcap |= bit(0) | (fp >= 1U ? bit(9) : 0);
    if (simd != 0xFU) hwcap |= bit(1) | (simd >= 1U ? bit(10) : 0);

    unsigned aes = field(isar0, 4);
    if (aes >= 1U) hwcap |= bit(3);
    if (aes >= 2U) hwcap |= bit(4);
    if (field(isar0, 8) >= 1U) hwcap |= bit(5);
    unsigned sha2 = field(isar0, 12);
    if (sha2 >= 1U) hwcap |= bit(6);
    if (sha2 >= 2U) hwcap |= bit(21);
    if (field(isar0, 16) >= 1U) hwcap |= bit(7);
    if (field(isar0, 20) >= 2U) hwcap |= bit(8);
    if (field(isar0, 28) >= 1U) hwcap |= bit(12);
    if (field(isar0, 32) >= 1U) hwcap |= bit(17);
    if (field(isar0, 36) >= 1U) hwcap |= bit(18);
    if (field(isar0, 40) >= 1U) hwcap |= bit(19);
    if (field(isar0, 44) >= 1U) hwcap |= bit(20);
    if (field(isar0, 48) >= 1U) hwcap |= bit(23);
    if (field(isar0, 52) >= 1U) hwcap |= bit(27);

    if (field(isar1, 0) >= 1U) hwcap |= bit(16);
    if (field(isar1, 12) >= 1U) hwcap |= bit(13);
    if (field(isar1, 16) >= 1U) hwcap |= bit(14);
    unsigned lrcpi = field(isar1, 20);
    if (lrcpi >= 1U) hwcap |= bit(15);
    if (lrcpi >= 2U) hwcap |= bit(26);
    if (field(isar1, 36) >= 1U) hwcap |= bit(29);
    return hwcap;
}

void arch_note_cpu_features(void) {
    uint64_t hwcap = current_hwcap();
    uint64_t seen = __atomic_load_n(&shared_hwcap, __ATOMIC_RELAXED);
    while (!__atomic_compare_exchange_n(&shared_hwcap, &seen, seen & hwcap, 0,
                                        __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
    }
}

uint64_t arch_elf_hwcap(void) {
    uint64_t hwcap = __atomic_load_n(&shared_hwcap, __ATOMIC_RELAXED);
    return hwcap == ~0ULL ? current_hwcap() : hwcap;
}

size_t arch_hwcap_names(char *out, size_t room) {
    uint64_t hwcap = arch_elf_hwcap();
    size_t used = 0;
    for (unsigned index = 0; index < sizeof(names) / sizeof(names[0]); index++) {
        if (!(hwcap & bit(index))) continue;
        for (const char *c = used ? " " : ""; *c && used + 1 < room; c++) out[used++] = *c;
        for (const char *c = names[index]; *c && used + 1 < room; c++) out[used++] = *c;
    }
    if (room) out[used] = '\0';
    return used;
}
