#include <stddef.h>
#include <stdint.h>

#include "arch.h"

#define FDT_MAGIC       0xd00dfeedU
#define FDT_BEGIN_NODE  1U
#define FDT_END_NODE    2U
#define FDT_PROP        3U
#define FDT_NOP         4U
#define FDT_END         9U

struct fdt_header {
    uint32_t magic;
    uint32_t totalsize;
    uint32_t off_dt_struct;
    uint32_t off_dt_strings;
    uint32_t off_mem_rsvmap;
    uint32_t version;
    uint32_t last_comp_version;
    uint32_t boot_cpuid_phys;
    uint32_t size_dt_strings;
    uint32_t size_dt_struct;
};

static uint32_t be32(const void *p) {
    const uint8_t *b = p;
    return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];
}

static uint64_t be64(const void *p) {
    return ((uint64_t)be32(p) << 32) | be32((const uint8_t *)p + 4);
}

static int name_is(const char *name, const char *prefix) {
    while (*prefix) if (*name++ != *prefix++) return 0;
    return 1;
}

const void *fdt_find(const void *hint) {
    const struct fdt_header *hdr = hint;
    if (hint && be32(&hdr->magic) == FDT_MAGIC) return hint;
    // Some boot paths leave x0 = 0; scan low RAM for the blob.
    for (uint64_t a = 0x40000000UL; a < 0x48000000UL; a += 0x10000UL) {
        hdr = (const struct fdt_header *)phys_to_virt(a);
        if (be32(&hdr->magic) == FDT_MAGIC) {
            uint32_t total = be32(&hdr->totalsize);
            if (total >= sizeof(*hdr) && total <= 0x200000U) return hdr;
        }
    }
    return NULL;
}

int fdt_probe(const void *dtb, uint64_t *ram_base, uint64_t *ram_bytes,
              uint32_t *cpu_count) {
    const struct fdt_header *hdr = dtb;
    if (!dtb || be32(&hdr->magic) != FDT_MAGIC) return -1;

    const uint8_t *base = dtb;
    const uint8_t *strings = base + be32(&hdr->off_dt_strings);
    const uint32_t *p = (const uint32_t *)(base + be32(&hdr->off_dt_struct));
    const uint32_t *end = p + be32(&hdr->size_dt_struct) / 4;

    uint64_t rambase = 0;
    uint64_t ram = 0;
    uint32_t cpus = 0;
    int in_memory = 0;

    while (p < end) {
        uint32_t token = be32(p++);
        if (token == FDT_BEGIN_NODE) {
            const char *node = (const char *)p;
            in_memory = name_is(node, "memory@") || name_is(node, "memory");
            if (name_is(node, "cpu@")) cpus++;
            size_t len = 0;
            while (node[len]) len++;
            p += (len + 4) / 4;                         // skip name, 4-byte aligned
        } else if (token == FDT_PROP) {
            uint32_t len = be32(p++);
            uint32_t nameoff = be32(p++);
            const char *pname = (const char *)(strings + nameoff);
            if (in_memory && name_is(pname, "reg") && len >= 16) {
                if (!ram) rambase = be64((const uint8_t *)p);   // first region base
                ram += be64((const uint8_t *)p + 8);            // ... and its size
            }
            p += (len + 3) / 4;                          // skip value, 4-byte aligned
        } else if (token == FDT_END) {
            break;
        }
    }

    if (ram_base) *ram_base = rambase;
    if (ram_bytes) *ram_bytes = ram;
    if (cpu_count) *cpu_count = cpus;
    return 0;
}
