#include <stddef.h>
#include <stdint.h>

#include <tunix/cpu.h>
#include <tunix/time.h>
#include <tunix/vmm.h>

#include "priv.h"

#define PDISP_OBJECTS    0x610010U
#define PDISP_INTR_0     0x610020U
#define PDISP_INTR_1     0x610024U
#define PDISP_INTR_EN_0  0x610028U
#define PDISP_INTR_EN_1  0x61002CU
#define PDISP_SUPERVISOR 0x610030U
#define PDISP_ERROR_ADDR 0x610080U
#define PDISP_ERROR_DATA 0x610084U
#define PDISP_CAP        0x610184U
#define PDISP_CORE_CTRL  0x610200U
#define PDISP_CORE_PUSH  0x610204U
#define PDISP_CORE_208   0x610208U
#define PDISP_CORE_20C   0x61020CU
#define PDISP_VGA_OWNER  0x6194E8U
#define PDISP_CORE_PUT   0x640000U
#define PDISP_CORE_GET   0x640004U
#define VBIOS_OWNS       0x00000100U
#define PMC_INTR         0x000100U
#define PMC_INTR_ENABLE  0x000140U
#define PMC_INTR_DISP    0x04000000U
#define INTR_1_VBLANK_0  0x00000004U
#define PCI_MSI_REARM    0x088068U

#define RAMHT_BYTES     0x1000U
#define RAMHT_BITS      9U
#define OBJECT_VRAM     0x1000U
#define OBJECT_SYNC     0x1020U
#define HANDLE_SYNC     0xF0000000U
#define HANDLE_VRAM     0xF0000001U
#define DMA_IN_MEMORY   0x3DU
#define DMA_TARGET_VRAM 0x00010000U
#define DMA_ACCESS_RW   0x00080000U
#define DMA_PRIV_US     (1U << 20)
#define DMA_PART_256    (1U << 16)

#define CORE_UPDATE                 0x0080U
#define CORE_SET_NOTIFIER_CONTROL   0x0084U
#define CORE_SET_CONTEXT_DMA_NOTIFY 0x0088U
#define HEAD_SET_OFFSET             0x0860U
#define HEAD_SET_SIZE               0x0868U
#define HEAD_SET_STORAGE            0x086CU
#define HEAD_SET_PARAMS             0x0870U
#define HEAD_SET_CONTEXT_DMA_ISO    0x0874U
#define HEAD_SET_VIEWPORT_POINT_IN  0x08C0U
#define HEAD_SET_VIEWPORT_SIZE_IN   0x08C8U
#define HEAD_SET_VIEWPORT_SIZE_OUT  0x08D8U
#define HEAD_SET_VIEWPORT_OUT_MIN   0x08DCU
#define HEAD_ARMED_PIXEL_CLOCK      0x610AD4U
#define HEAD_ARMED_SIZE_OUT         0x610B4CU
#define NOTIFY_ENABLE               0x80000000U
#define STORAGE_PITCH_LAYOUT        0x00100000U
#define FORMAT_A8R8G8B8             0xCFU
#define PIXEL_CLOCK_NOT_DRIVER      0x02000000U
#define SURFACE_PITCH_ALIGN         256U
#define PUSH_USABLE_BYTES           0xFC0U
#define PUSH_JUMP_TO_START          0x20000000U
#define FLIP_BATCH_BYTES            32U
#define TIMEOUT_NS                  2000000000ULL
#define SUPERVISOR_TIMEOUT_NS       5000000000ULL

#define VPLL_COEFFICIENTS 0x614104U
#define VPLL_FRACTION     0x614108U
#define HEAD_RG_CLOCK     0x614200U
#define SOR_CLOCK         0x614300U

extern void kprintf(const char *fmt, ...);

struct method_mirror {
    uint16_t method;
    uint32_t reg;
};

struct method_list {
    uint16_t method_stride;
    uint16_t reg_stride;
    uint8_t count;
    const struct method_mirror *entries;
    uint8_t entry_count;
};

static const struct method_mirror dac_methods[] = {
    {0x0400, 0x610B58}, {0x0404, 0x610BDC}, {0x0420, 0x610BC4}};
static const struct method_mirror sor_methods[] = {{0x0600, 0x610794}};
static const struct method_mirror pior_methods[] = {{0x0700, 0x610B80}};
static const struct method_mirror head_methods[] = {
    {0x0800, 0x610AD8}, {0x0804, 0x610AD0}, {0x0808, 0x610A48}, {0x080C, 0x610A78},
    {0x0810, 0x610AC0}, {0x0814, 0x610AF8}, {0x0818, 0x610B00}, {0x081C, 0x610AE8},
    {0x0820, 0x610AF0}, {0x0824, 0x610B08}, {0x0828, 0x610B10}, {0x082C, 0x610A68},
    {0x0830, 0x610A60}, {0x0838, 0x610A40}, {0x0840, 0x610A24}, {0x0844, 0x610A2C},
    {0x0848, 0x610AA8}, {0x084C, 0x610AB0}, {0x085C, 0x610C5C}, {0x0860, 0x610A84},
    {0x0864, 0x610A90}, {0x0868, 0x610B18}, {0x086C, 0x610B20}, {0x0870, 0x610AC8},
    {0x0874, 0x610A38}, {0x0878, 0x610C50}, {0x0880, 0x610A58}, {0x0884, 0x610A9C},
    {0x089C, 0x610C68}, {0x08A0, 0x610A70}, {0x08A4, 0x610A50}, {0x08A8, 0x610AE0},
    {0x08C0, 0x610B28}, {0x08C4, 0x610B30}, {0x08C8, 0x610B40}, {0x08D4, 0x610B38},
    {0x08D8, 0x610B48}, {0x08DC, 0x610B50}, {0x0900, 0x610A18}, {0x0904, 0x610AB8},
    {0x0910, 0x610C70}, {0x0914, 0x610C78}};

static const struct method_list core_lists[] = {
    {0x80, 8, 3, dac_methods, sizeof(dac_methods) / sizeof(dac_methods[0])},
    {0x40, 8, 4, sor_methods, sizeof(sor_methods) / sizeof(sor_methods[0])},
    {0x40, 8, 3, pior_methods, sizeof(pior_methods) / sizeof(pior_methods[0])},
    {0x400, 0x540, 2, head_methods, sizeof(head_methods) / sizeof(head_methods[0])},
};

int nv50_wait_clear(const struct nv50_device *gpu, uint32_t offset, uint32_t mask) {
    uint64_t deadline = time_uptime_ns() + TIMEOUT_NS;
    while (nv50_rd32(gpu, offset) & mask) {
        if (time_uptime_ns() > deadline) return -1;
        cpu_relax();
    }
    return 0;
}

static uint32_t vram_rd32(const struct nv50_device *gpu, uint32_t vram) {
    return nv50_rd32(gpu, PRAMIN_WINDOW + (vram - VRAM_INSTANCE));
}

static void vram_wr32(const struct nv50_device *gpu, uint32_t vram, uint32_t value) {
    nv50_wr32(gpu, PRAMIN_WINDOW + (vram - VRAM_INSTANCE), value);
}

static int vram_flush(const struct nv50_device *gpu) {
    nv50_wr32(gpu, PBUS_FLUSH, 0x00000001U);
    return nv50_wait_clear(gpu, PBUS_FLUSH, 0x00000002U);
}

static int check_bar1(const struct nv50_device *gpu) {
    vram_wr32(gpu, VRAM_CHECK, 0x54554E49U);
    if (vram_flush(gpu) != 0) return -1;
    volatile const uint32_t *linear =
        (volatile const uint32_t *)vmm_map_device(gpu->bar1 + VRAM_CHECK, 0x1000U);
    return linear && *linear == 0x54554E49U ? 0 : -1;
}

static uint32_t ramht_hash(uint32_t handle) {
    uint32_t hash = 0;
    while (handle) {
        hash ^= handle & ((1U << RAMHT_BITS) - 1U);
        handle >>= RAMHT_BITS;
    }
    return hash;
}

static void write_ctxdma(const struct nv50_device *gpu, uint32_t object, uint32_t start,
                         uint32_t limit) {
    uint32_t base = VRAM_INSTANCE + object;
    vram_wr32(gpu, base + 0x00U, DMA_PRIV_US | DMA_ACCESS_RW | DMA_TARGET_VRAM | DMA_IN_MEMORY);
    vram_wr32(gpu, base + 0x04U, limit);
    vram_wr32(gpu, base + 0x08U, start);
    vram_wr32(gpu, base + 0x0CU, 0);
    vram_wr32(gpu, base + 0x10U, 0);
    vram_wr32(gpu, base + 0x14U, DMA_PART_256);
}

static void bind_object(const struct nv50_device *gpu, uint32_t handle, uint32_t object) {
    uint32_t slot = ramht_hash(handle);
    while (vram_rd32(gpu, VRAM_INSTANCE + slot * 8U)) slot = (slot + 1U) % (RAMHT_BYTES / 8U);
    vram_wr32(gpu, VRAM_INSTANCE + slot * 8U, handle);
    vram_wr32(gpu, VRAM_INSTANCE + slot * 8U + 4U, object << 10);
}

static int build_objects(const struct nv50_device *gpu) {
    for (uint32_t offset = 0; offset < VRAM_INSTANCE_BYTES; offset += 4U)
        vram_wr32(gpu, VRAM_INSTANCE + offset, 0);
    for (uint32_t offset = 0; offset < 0x2000U; offset += 4U) vram_wr32(gpu, VRAM_PUSH + offset, 0);
    write_ctxdma(gpu, OBJECT_VRAM, 0, gpu->vram_bytes - 1U);
    write_ctxdma(gpu, OBJECT_SYNC, VRAM_SYNC, VRAM_SYNC + 0xFFFU);
    bind_object(gpu, HANDLE_VRAM, OBJECT_VRAM);
    bind_object(gpu, HANDLE_SYNC, OBJECT_SYNC);
    if (vram_flush(gpu) != 0) return -1;
    return vram_rd32(gpu, VRAM_INSTANCE + OBJECT_SYNC + 0x08U) == VRAM_SYNC ? 0 : -1;
}

static int take_over(const struct nv50_device *gpu) {
    nv50_wr32(gpu, PDISP_CAP, nv50_rd32(gpu, 0x614004U));
    for (uint32_t head = 0; head < 2; head++)
        for (uint32_t word = 0; word < 4; word++)
            nv50_wr32(gpu, 0x610190U + head * 0x10U + word * 4U,
                      nv50_rd32(gpu, 0x616100U + head * 0x800U + word * 4U));
    uint32_t cap = nv50_rd32(gpu, PDISP_CAP);
    for (uint32_t index = 0; index < 3; index++)
        if (cap & (0x00100000U << index))
            nv50_wr32(gpu, 0x6101D0U + index * 4U, nv50_rd32(gpu, 0x61A000U + index * 0x800U));
    for (uint32_t index = 0; index < 4; index++)
        if (cap & (0x01000000U << index))
            nv50_wr32(gpu, 0x6101E0U + index * 4U, nv50_rd32(gpu, 0x61C000U + index * 0x800U));
    for (uint32_t index = 0; index < 3; index++)
        if (cap & (0x10000000U << index))
            nv50_wr32(gpu, 0x6101F0U + index * 4U, nv50_rd32(gpu, 0x61E000U + index * 0x800U));

    if (nv50_rd32(gpu, PDISP_INTR_1) & VBIOS_OWNS) {
        nv50_wr32(gpu, PDISP_INTR_1, VBIOS_OWNS);
        nv50_mask(gpu, PDISP_VGA_OWNER, 0x00000001U, 0);
        if (nv50_wait_clear(gpu, PDISP_VGA_OWNER, 0x00000002U) != 0) {
            kprintf("NV50: the vbios did not let go of the display\n");
            return -1;
        }
    }
    nv50_wr32(gpu, PDISP_OBJECTS, (VRAM_INSTANCE >> 8) | 9U);
    return 0;
}

static int start_core(struct nv50_device *gpu) {
    if ((nv50_rd32(gpu, PDISP_CORE_CTRL) & 0x009F0000U) == 0x00020000U)
        nv50_mask(gpu, PDISP_CORE_CTRL, 0x00800000U, 0x00800000U);
    if ((nv50_rd32(gpu, PDISP_CORE_CTRL) & 0x003F0000U) == 0x00030000U)
        nv50_mask(gpu, PDISP_CORE_CTRL, 0x00600000U, 0x00600000U);
    nv50_wr32(gpu, PDISP_CORE_PUSH, (VRAM_PUSH >> 8) | 1U);
    nv50_wr32(gpu, PDISP_CORE_208, 0x00010000U);
    nv50_wr32(gpu, PDISP_CORE_20C, 0);
    nv50_mask(gpu, PDISP_CORE_CTRL, 0x00000010U, 0x00000010U);
    nv50_wr32(gpu, PDISP_CORE_PUT, 0);
    nv50_wr32(gpu, PDISP_CORE_CTRL, 0x01000013U);
    gpu->push_at = 0;
    gpu->push_overflow = 0;
    if (nv50_wait_clear(gpu, PDISP_CORE_CTRL, 0x80000000U) != 0) {
        kprintf("NV50: the core channel did not start, ctrl %x\n", nv50_rd32(gpu, PDISP_CORE_CTRL));
        return -1;
    }
    return 0;
}

static void push(struct nv50_device *gpu, uint32_t method, uint32_t data) {
    if (gpu->push_at + 8U > PUSH_USABLE_BYTES) {
        gpu->push_overflow = 1;
        return;
    }
    vram_wr32(gpu, VRAM_PUSH + gpu->push_at, (1U << 18) | method);
    vram_wr32(gpu, VRAM_PUSH + gpu->push_at + 4U, data);
    gpu->push_at += 8U;
}

static int kick(struct nv50_device *gpu) {
    if (gpu->push_overflow || vram_flush(gpu) != 0) return -1;
    nv50_wr32(gpu, PDISP_CORE_PUT, gpu->push_at);
    uint64_t deadline = time_uptime_ns() + TIMEOUT_NS;
    while (nv50_rd32(gpu, PDISP_CORE_GET) != gpu->push_at) {
        if (time_uptime_ns() > deadline) return -1;
        cpu_relax();
    }
    return 0;
}

int nv50_disp_init(struct nv50_device *gpu) {
    gpu->pramin_saved = nv50_rd32(gpu, PBUS_PRAMIN_BASE);
    nv50_wr32(gpu, PBUS_PRAMIN_BASE, VRAM_INSTANCE >> 16);
    if (check_bar1(gpu) != 0) {
        kprintf("NV50: bar1 does not reach vram linearly\n");
        return -1;
    }
    if (build_objects(gpu) != 0) {
        kprintf("NV50: the display objects did not reach vram\n");
        return -1;
    }
    return take_over(gpu) == 0 && start_core(gpu) == 0 ? 0 : -1;
}

static uint32_t sanitize_armed(uint32_t method, uint32_t value) {
    if (method < 0x0800U || method >= 0x0C00U + 0x400U) return value;
    uint32_t field = (method - 0x0800U) % 0x400U;
    if (field == 0x004U) return value & ~PIXEL_CLOCK_NOT_DRIVER;
    if (field == 0x024U) return 1U;
    return value;
}

static unsigned restore_armed_state(struct nv50_device *gpu) {
    unsigned restored = 0;
    for (size_t list = 0; list < sizeof(core_lists) / sizeof(core_lists[0]); list++) {
        const struct method_list *methods = &core_lists[list];
        for (uint8_t instance = 0; instance < methods->count; instance++) {
            for (uint8_t entry = 0; entry < methods->entry_count; entry++) {
                uint32_t reg = methods->entries[entry].reg + instance * methods->reg_stride;
                uint32_t method =
                    methods->entries[entry].method + instance * methods->method_stride;
                uint32_t armed = sanitize_armed(method, nv50_rd32(gpu, reg + 4U));
                if (nv50_rd32(gpu, reg) == armed) continue;
                push(gpu, method, armed);
                restored++;
            }
        }
    }
    return restored;
}

static void report_error(const struct nv50_device *gpu) {
    kprintf("NV50: intr0 %x error %x data %x\n", nv50_rd32(gpu, PDISP_INTR_0),
            nv50_rd32(gpu, PDISP_ERROR_ADDR), nv50_rd32(gpu, PDISP_ERROR_DATA));
}

static int service_supervisors(struct nv50_device *gpu) {
    const struct nv50_lvds *lvds = &gpu->lvds;
    uint64_t deadline = time_uptime_ns() + SUPERVISOR_TIMEOUT_NS;
    while (time_uptime_ns() < deadline) {
        if (vram_rd32(gpu, VRAM_SYNC) & 1U) return 0;
        uint32_t pending = nv50_rd32(gpu, PDISP_INTR_1) & 0x70U;
        if (!pending) {
            cpu_relax();
            continue;
        }
        uint32_t super = nv50_rd32(gpu, PDISP_SUPERVISOR);
        nv50_wr32(gpu, PDISP_INTR_1, pending);
        if (pending & 0x20U) {
            if (super & 0x80U) (void)nv50_bios_run(gpu, lvds->off_int2);
            if (super & 0x200U) {
                nv50_wr32(gpu, VPLL_COEFFICIENTS, gpu->vpll_coefficients);
                nv50_wr32(gpu, VPLL_FRACTION, gpu->vpll_fraction);
            }
            if (super & 0x80U) {
                (void)nv50_bios_run(gpu, lvds->on_int2);
                nv50_mask(gpu, HEAD_RG_CLOCK, 0x0000000FU, 0);
                nv50_mask(gpu, SOR_CLOCK + lvds->or_index * 0x800U, 0x00000707U, 0);
            }
        } else if (pending & 0x40U) {
            if (super & 0x80U) (void)nv50_bios_run(gpu, lvds->on_int3);
        } else if (super & 0xA0U) {
            (void)nv50_bios_run(gpu, lvds->off_int1);
        }
        nv50_wr32(gpu, PDISP_SUPERVISOR, 0x80000000U);
    }
    return -1;
}

int nv50_disp_modeset(struct nv50_device *gpu) {
    uint32_t size_out = nv50_rd32(gpu, HEAD_ARMED_SIZE_OUT);
    uint32_t clock = nv50_rd32(gpu, HEAD_ARMED_PIXEL_CLOCK) & 0x3FFFFFU;
    gpu->width = size_out & 0x7FFFU;
    gpu->height = (size_out >> 16) & 0x7FFFU;
    gpu->pitch = (gpu->width * 4U + SURFACE_PITCH_ALIGN - 1U) & ~(SURFACE_PITCH_ALIGN - 1U);
    if (gpu->width < 640U || gpu->height < 480U || !clock) {
        kprintf("NV50: the vbios mode %ux%u at %u khz is not usable\n", gpu->width, gpu->height,
                clock);
        return -1;
    }
    if (nv50_bios_find_lvds(gpu, clock) != 0) return -1;
    gpu->vpll_coefficients = nv50_rd32(gpu, VPLL_COEFFICIENTS);
    gpu->vpll_fraction = nv50_rd32(gpu, VPLL_FRACTION);

    uint32_t size = (gpu->height << 16) | gpu->width;
    vram_wr32(gpu, VRAM_SYNC, 0);
    push(gpu, CORE_SET_CONTEXT_DMA_NOTIFY, HANDLE_SYNC);
    unsigned restored = restore_armed_state(gpu);
    push(gpu, HEAD_SET_OFFSET, VRAM_SURFACE >> 8);
    push(gpu, HEAD_SET_SIZE, size);
    push(gpu, HEAD_SET_STORAGE, STORAGE_PITCH_LAYOUT | gpu->pitch);
    push(gpu, HEAD_SET_PARAMS, FORMAT_A8R8G8B8 << 8);
    push(gpu, HEAD_SET_CONTEXT_DMA_ISO, HANDLE_VRAM);
    push(gpu, HEAD_SET_VIEWPORT_POINT_IN, 0);
    push(gpu, HEAD_SET_VIEWPORT_SIZE_IN, size);
    push(gpu, HEAD_SET_VIEWPORT_SIZE_OUT, size);
    push(gpu, HEAD_SET_VIEWPORT_OUT_MIN, size);
    push(gpu, CORE_SET_NOTIFIER_CONTROL, NOTIFY_ENABLE);
    push(gpu, CORE_UPDATE, 0);
    push(gpu, CORE_SET_NOTIFIER_CONTROL, 0);
    if (kick(gpu) != 0 || service_supervisors(gpu) != 0) {
        kprintf("NV50: the modeset did not complete\n");
        report_error(gpu);
        nv50_wr32(gpu, PBUS_PRAMIN_BASE, gpu->pramin_saved);
        return -1;
    }
    gpu->front = VRAM_SURFACE;
    kprintf("NV50: %ux%u at %u khz, %u methods restored\n", gpu->width, gpu->height, clock,
            restored);
    return 0;
}

int nv50_disp_flip(struct nv50_device *gpu, uint32_t vram) {
    if (gpu->flip_pending) return -1;
    if (gpu->push_at + FLIP_BATCH_BYTES + 4U > PUSH_USABLE_BYTES) {
        vram_wr32(gpu, VRAM_PUSH + gpu->push_at, PUSH_JUMP_TO_START);
        gpu->push_at = 0;
    }
    vram_wr32(gpu, VRAM_SYNC, 0);
    push(gpu, HEAD_SET_OFFSET, vram >> 8);
    push(gpu, CORE_SET_NOTIFIER_CONTROL, NOTIFY_ENABLE);
    push(gpu, CORE_UPDATE, 0);
    push(gpu, CORE_SET_NOTIFIER_CONTROL, 0);
    if (kick(gpu) != 0) {
        if (!gpu->warned++) kprintf("NV50: a flip was not consumed\n");
        return -1;
    }
    gpu->front = vram;
    gpu->flip_pending = 1;
    return 0;
}

int nv50_disp_flip_idle(struct nv50_device *gpu, int may_service) {
    if (!gpu->flip_pending) return 1;
    if (vram_rd32(gpu, VRAM_SYNC) & 1U) {
        gpu->flip_pending = 0;
        return 1;
    }
    if (!may_service) return 0;
    if (nv50_rd32(gpu, PDISP_INTR_0) & 0x001F0000U) {
        if (!gpu->warned++) report_error(gpu);
        gpu->flip_pending = 0;
        return 1;
    }
    if (nv50_rd32(gpu, PDISP_INTR_1) & 0x70U) {
        if (!gpu->warned++) kprintf("NV50: a flip raised a supervisor\n");
        (void)service_supervisors(gpu);
        gpu->flip_pending = 0;
        return 1;
    }
    return 0;
}

void nv50_disp_vblank_start(struct nv50_device *gpu) {
    nv50_wr32(gpu, PDISP_INTR_EN_0, 0);
    nv50_wr32(gpu, PDISP_INTR_1, INTR_1_VBLANK_0);
    nv50_wr32(gpu, PDISP_INTR_EN_1, INTR_1_VBLANK_0);
    nv50_wr32(gpu, PMC_INTR_ENABLE, 1);
}

void nv50_disp_vblank_stop(struct nv50_device *gpu) {
    nv50_wr32(gpu, PMC_INTR_ENABLE, 0);
    nv50_wr32(gpu, PDISP_INTR_EN_1, 0);
}

int nv50_disp_interrupt(struct nv50_device *gpu) {
    uint32_t pending = nv50_rd32(gpu, PMC_INTR);
    if (!pending) return 0;
    if (pending & ~PMC_INTR_DISP) return -1;
    if (!(nv50_rd32(gpu, PDISP_INTR_1) & INTR_1_VBLANK_0)) return -1;
    nv50_wr32(gpu, PDISP_INTR_1, INTR_1_VBLANK_0);
    *(volatile uint8_t *)(gpu->bar0 + PCI_MSI_REARM) = 0xFFU;
    return 1;
}
