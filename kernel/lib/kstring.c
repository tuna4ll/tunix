#include <stddef.h>
#include <stdint.h>
#include "../include/kstring.h"

/* The string instructions rather than a byte loop in C. */
/* These two are what every copy in the kernel goes through -- a user copy, a
   pipe, a page table being zeroed, a struct assignment the compiler turned into
   a call -- and a loop that moves one byte per iteration was costing about a
   nanosecond a byte. */
/* The direction flag is clear whenever kernel code runs: FMASK clears it on the
   syscall path and the interrupt stubs `cld` on theirs. */
/* General registers only, which is what -mgeneral-regs-only asks for. */
void *memset(void *dst, int value, size_t count) {
    void *out = dst;
    __asm__ volatile("rep stosb"
                     : "+D"(out), "+c"(count)
                     : "a"((uint8_t)value)
                     : "memory");
    return dst;
}

void *memcpy(void *dst, const void *src, size_t count) {
    void *out = dst;
    const void *in = src;
    __asm__ volatile("rep movsb"
                     : "+D"(out), "+S"(in), "+c"(count)
                     :
                     : "memory");
    return dst;
}

/* Only the descending case is written out, because the ascending one is a plain
   copy and the flag this sets has to be cleared again before any C runs. */
void *memmove(void *dst, const void *src, size_t count) {
    uint8_t *out = (uint8_t *)dst;
    const uint8_t *in = (const uint8_t *)src;
    if (out < in || !count) return memcpy(dst, src, count);
    void *last_out = out + count - 1;
    const void *last_in = in + count - 1;
    __asm__ volatile("std; rep movsb; cld"
                     : "+D"(last_out), "+S"(last_in), "+c"(count)
                     :
                     : "memory");
    return dst;
}

size_t strlen(const char *str) {
    size_t length = 0;
    while (str && str[length]) length++;
    return length;
}

int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t count) {
    while (count && *a && *a == *b) { a++; b++; count--; }
    if (!count) return 0;
    return (unsigned char)*a - (unsigned char)*b;
}

char *strncpy(char *dst, const char *src, size_t count) {
    size_t i = 0;
    for (; i < count && src[i]; i++) dst[i] = src[i];
    for (; i < count; i++) dst[i] = '\0';
    return dst;
}
