#ifndef TUNIX_NV50_H
#define TUNIX_NV50_H

#include <stddef.h>
#include <stdint.h>

struct pci_device;

size_t nv50_display_probe(const struct pci_device *device, uint64_t bar0, const uint8_t *vbios,
                          uint32_t vbios_bytes, char *log, size_t capacity);

#endif
