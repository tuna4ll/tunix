// What nouveau's PGRAPH context generator (ctxnv50.c, ctxnv40.h, kept as
// nouveau has them) needs of nvkm, in Tunix's terms.
#ifndef TUNIX_NV50_NVKM_COMPAT_H
#define TUNIX_NV50_NVKM_COMPAT_H

#include <stddef.h>
#include <stdint.h>

#include <tunix/errno.h>
#include <tunix/heap.h>
#include <tunix/kprintf.h>

#include "priv.h"

// Linux builds nouveau without these warnings; its code is kept as it is.
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

#define GFP_KERNEL 0
#define kmalloc(bytes, flags) kmalloc(bytes)
#define BUG_ON(condition)                                                                      \
    do {                                                                                       \
        if (condition) kprintf("NV50: grctx: %s at %s:%d\n", #condition, __FILE__, __LINE__);  \
    } while (0)

enum { NVKM_RAM_TYPE_OTHER, NVKM_RAM_TYPE_GDDR5 };

struct nvkm_ram {
    int type;
};

struct nvkm_fb {
    struct nvkm_ram *ram;
};

struct nvkm_device {
    int chipset;
    struct nvkm_fb *fb;
    struct nv50_device *gpu;
};

// A context image in VRAM, written through BAR1.
struct nvkm_gpuobj {
    volatile uint32_t *map;
};

static inline u32 nvkm_rd32(struct nvkm_device *device, u32 reg) {
    return nv50_rd32(device->gpu, reg);
}

static inline void nvkm_wr32(struct nvkm_device *device, u32 reg, u32 value) {
    nv50_wr32(device->gpu, reg, value);
}

static inline void nvkm_wo32(struct nvkm_gpuobj *object, u32 offset, u32 value) {
    object->map[offset / 4U] = value;
}

int nv50_grctx_init(struct nvkm_device *device, u32 *size);
void nv50_grctx_fill(struct nvkm_device *device, struct nvkm_gpuobj *mem);

#endif
