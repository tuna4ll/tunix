#include <stddef.h>
#include <stdint.h>
#include "../include/kstring.h"

#if defined(__x86_64__)

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

#elif !defined(__aarch64__)

void *memset(void *dst, int value, size_t count) {
    volatile uint8_t *out = (volatile uint8_t *)dst;
    while (count--) *out++ = (uint8_t)value;
    return dst;
}

void *memcpy(void *dst, const void *src, size_t count) {
    volatile uint8_t *out = (volatile uint8_t *)dst;
    const volatile uint8_t *in = (const volatile uint8_t *)src;
    while (count--) *out++ = *in++;
    return dst;
}

void *memmove(void *dst, const void *src, size_t count) {
    volatile uint8_t *out = (volatile uint8_t *)dst;
    const volatile uint8_t *in = (const volatile uint8_t *)src;
    if (out < in || !count) return memcpy(dst, src, count);
    out += count;
    in += count;
    while (count--) *--out = *--in;
    return dst;
}

#endif

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
