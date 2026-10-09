#ifndef TUNIX_NV50_H
#define TUNIX_NV50_H

#include <stddef.h>

struct boot_framebuffer_info;

int nv50_early_init(const struct boot_framebuffer_info *boot, struct boot_framebuffer_info *out);
const char *nv50_early_log(size_t *bytes);

#endif
