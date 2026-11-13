#ifndef TUNIX_KASAN_H
#define TUNIX_KASAN_H

#include <stddef.h>
#include <stdint.h>

#define KASAN_SHADOW_BASE 0xFFFFFF8000000000ULL
#define KASAN_FREE_SPACE  0xFEU
#define KASAN_FREED       0xFBU
#define KASAN_REDZONE     0xFCU
#define KASAN_REDZONE_MIN 16U
#define KASAN_QUARANTINE  (32ULL * 1024ULL * 1024ULL)

#if defined(TUNIX_KASAN)
int kasan_cover(uint64_t start, uint64_t length);
void kasan_poison(uint64_t start, uint64_t length, uint8_t value);
void kasan_unpoison(uint64_t start, uint64_t length);
void kasan_report_free(const void *pointer, uint64_t caller, int double_free);
void kasan_check_range(uint64_t address, size_t size, int write, uint64_t caller);
uint32_t kasan_report_count(void);
void kasan_selftest(void);
#endif

#endif
