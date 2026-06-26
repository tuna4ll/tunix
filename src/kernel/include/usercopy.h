#ifndef TUNIX_USERCOPY_H
#define TUNIX_USERCOPY_H

#include <stddef.h>
#include <stdint.h>

int copy_from_user(void *destination, uint64_t user_source, size_t length);
int copy_to_user(uint64_t user_destination, const void *source, size_t length);
int copy_string_from_user(char *destination, size_t capacity, uint64_t user_source);

#endif
