#ifndef TUNIX_NV50_PRIV_H
#define TUNIX_NV50_PRIV_H

#include <stddef.h>
#include <stdint.h>

#define PBUS_PRAMIN_BASE  0x001700U
#define PBUS_FLUSH        0x070000U
#define PRAMIN_WINDOW     0x700000U
#define PDISP_VBIOS_PTR   0x619F04U
#define VBIOS_IMAGE_BYTES 0x20000U

#define VRAM_INSTANCE       0x00800000U
#define VRAM_INSTANCE_BYTES 0x10000U
#define VRAM_PUSH           0x00810000U
#define VRAM_SYNC           0x00811000U
#define VRAM_CHECK          0x00820000U
#define VRAM_SURFACE        0x01000000U

struct nv50_lvds {
    uint16_t hash_type;
    uint16_t hash_mask;
    uint8_t or_index;
    uint8_t link;
    uint16_t off_int1;
    uint16_t off_int2;
    uint16_t on_int2;
    uint16_t on_int3;
};

struct nv50_device {
    uint64_t bar0;
    uint64_t bar1;
    uint32_t vram_bytes;
    uint32_t pramin_saved;
    const uint8_t *vbios;
    uint32_t vbios_bytes;
    uint32_t push_at;
    int push_overflow;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t vpll_coefficients;
    uint32_t vpll_fraction;
    struct nv50_lvds lvds;
    uint32_t front;
    int flip_pending;
    int warned;
};

static inline uint32_t nv50_rd32(const struct nv50_device *gpu, uint32_t offset) {
    return *(volatile const uint32_t *)(gpu->bar0 + offset);
}

static inline void nv50_wr32(const struct nv50_device *gpu, uint32_t offset, uint32_t value) {
    *(volatile uint32_t *)(gpu->bar0 + offset) = value;
}

static inline void nv50_mask(const struct nv50_device *gpu, uint32_t offset, uint32_t mask,
                             uint32_t value) {
    nv50_wr32(gpu, offset, (nv50_rd32(gpu, offset) & ~mask) | value);
}

int nv50_wait_clear(const struct nv50_device *gpu, uint32_t offset, uint32_t mask);

int nv50_bios_read(struct nv50_device *gpu, uint8_t *image, uint32_t capacity);
int nv50_bios_find_lvds(struct nv50_device *gpu, uint32_t khz);
int nv50_bios_run(struct nv50_device *gpu, uint16_t script);

int nv50_disp_init(struct nv50_device *gpu);
int nv50_disp_modeset(struct nv50_device *gpu);
int nv50_disp_flip(struct nv50_device *gpu, uint32_t vram);
int nv50_disp_flip_idle(struct nv50_device *gpu, int may_service);

#endif
