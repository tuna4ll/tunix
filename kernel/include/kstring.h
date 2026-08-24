#ifndef TUNIX_KSTRING_H
#define TUNIX_KSTRING_H

#include <stddef.h>

void *memset(void *dst, int value, size_t count);
void *memcpy(void *dst, const void *src, size_t count);
void *memmove(void *dst, const void *src, size_t count);
size_t strlen(const char *str);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t count);
char *strncpy(char *dst, const char *src, size_t count);

#endif
