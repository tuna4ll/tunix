#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

int tunix_tcc_header_probe(const char *text) {
    return text ? (int)strlen(text) + (int)sizeof(uint64_t) : 0;
}
