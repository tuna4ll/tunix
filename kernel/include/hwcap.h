#ifndef TUNIX_HWCAP_H
#define TUNIX_HWCAP_H

#include <stddef.h>
#include <stdint.h>

#if defined(__aarch64__)
void arch_note_cpu_features(void);
uint64_t arch_elf_hwcap(void);
size_t arch_hwcap_names(char *out, size_t room);
#else
static inline uint64_t arch_elf_hwcap(void) {
    return 0;
}
#endif

#endif
