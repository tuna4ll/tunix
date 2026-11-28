#include <stddef.h>
#include <stdint.h>

#include <tunix/cpu.h>
#include <tunix/time.h>

#pragma GCC diagnostic push
#include "nvkm_compat.h"
#pragma GCC diagnostic pop
#include "priv.h"
#include <tunix/kprintf.h>

// PGRAPH, the graphics engine, brought up as nouveau's nvkm/engine/gr/nv50.c
// (MIT) does on GT215, with the context program and initial context values
// nouveau's ctxnv50.c generates; channel 1 gets a context and a 2D object.

#define PMC_ENABLE    0x000200U
#define PMC_ENABLE_GR 0x00201000U // gt215_mc_reset's
#define GR_INTR       0x400100U
#define GR_TRAP       0x400108U
#define GR_INTR_EN    0x40013CU
#define GR_STATUS     0x400700U

#define INST_GR_2D  0x5700U // the 2D object's 16 bytes in the channel's instance
#define ENG_GR      0x0020U // where g84_ectx_bind points PGRAPH at its context
#define ENGINE_GR   1U      // PGRAPH's place on g98's runlist
#define HANDLE_2D   0xF0000030U
#define CLASS_2D    0x502DU
#define SUBC_2D     2U

static uint32_t context_bytes;
static int chipset;
static int gddr5;

uint32_t nv50_gr_context_bytes(void) { return context_bytes; }

// A channel's context: the generator's values over zeroes.
uint32_t nv50_gr_context_new(struct nv50_device *gpu) {
    if (!context_bytes) return 0;
    uint32_t context = nv50_vram_alloc(&gpu->vram, context_bytes, 0x1000U);
    volatile uint32_t *map = context ? nv50_vram_map(gpu, context, context_bytes) : NULL;
    if (!map) {
        if (context) nv50_vram_free(&gpu->vram, context, context_bytes);
        return 0;
    }
    for (uint32_t word = 0; word < context_bytes / 4U; word++) map[word] = 0;
    struct nvkm_ram ram = {gddr5 ? NVKM_RAM_TYPE_GDDR5 : NVKM_RAM_TYPE_OTHER};
    struct nvkm_fb fb = {&ram};
    struct nvkm_device device = {chipset, &fb, gpu};
    struct nvkm_gpuobj image = {map};
    nv50_grctx_fill(&device, &image);
    (void)map[0];
    return context;
}

void nv50_gr_context_free(struct nv50_device *gpu, uint32_t context) {
    if (context) nv50_vram_free(&gpu->vram, context, context_bytes);
}

// Bound as g84_ectx_bind binds an engine's context.
void nv50_gr_context_bind(volatile uint32_t *inst, uint32_t context) {
    inst[(ENG_GR + 0x00U) / 4U] = 0x00190000U;
    inst[(ENG_GR + 0x04U) / 4U] = context + context_bytes - 1U;
    inst[(ENG_GR + 0x08U) / 4U] = context;
    inst[(ENG_GR + 0x0CU) / 4U] = context;
    inst[(ENG_GR + 0x10U) / 4U] = 0;
    inst[(ENG_GR + 0x14U) / 4U] = 0;
}

int nv50_gr_start(struct nv50_device *gpu) {
    struct nvkm_ram ram = {(nv50_rd32(gpu, 0x100714U) & 7U) == 4U ? NVKM_RAM_TYPE_GDDR5
                                                                   : NVKM_RAM_TYPE_OTHER};
    struct nvkm_fb fb = {&ram};
    struct nvkm_device device = {(int)((nv50_rd32(gpu, 0) >> 20) & 0x1FFU), &fb, gpu};
    chipset = device.chipset;
    gddr5 = ram.type == NVKM_RAM_TYPE_GDDR5;

    nv50_mask(gpu, PMC_ENABLE, PMC_ENABLE_GR, 0);
    nv50_mask(gpu, PMC_ENABLE, PMC_ENABLE_GR, PMC_ENABLE_GR);
    nv50_wr32(gpu, 0x40008CU, 0x00000004U); // hardware context switches
    nv50_wr32(gpu, 0x400804U, 0xC0000000U);
    nv50_wr32(gpu, 0x406800U, 0xC0000000U);
    nv50_wr32(gpu, 0x400C04U, 0xC0000000U);
    nv50_wr32(gpu, 0x401800U, 0xC0000000U);
    nv50_wr32(gpu, 0x405018U, 0xC0000000U);
    nv50_wr32(gpu, 0x402000U, 0xC0000000U);
    uint32_t units = nv50_rd32(gpu, 0x001540U);
    for (uint32_t unit = 0; unit < 16U; unit++) {
        if (!(units & (1U << unit))) continue;
        nv50_wr32(gpu, 0x408600U + (unit << 11), 0xC0000000U);
        nv50_wr32(gpu, 0x408708U + (unit << 11), 0xC0000000U);
        nv50_wr32(gpu, 0x40831CU + (unit << 11), 0xC0000000U);
    }
    nv50_wr32(gpu, GR_TRAP, 0xFFFFFFFFU);
    nv50_wr32(gpu, 0x400138U, 0xFFFFFFFFU);
    nv50_wr32(gpu, GR_INTR, 0xFFFFFFFFU);
    // nouveau turns them all on; here only the display interrupts, and
    // PGRAPH's errors are read from GR_INTR and GR_TRAP.
    nv50_wr32(gpu, GR_INTR_EN, 0);
    nv50_wr32(gpu, 0x400500U, 0x00010001U);
    if (nv50_grctx_init(&device, &context_bytes) != 0) return -1;
    nv50_wr32(gpu, 0x400824U, 0);
    nv50_wr32(gpu, 0x400828U, 0);
    nv50_wr32(gpu, 0x40082CU, 0);
    nv50_wr32(gpu, 0x400830U, 0);
    nv50_wr32(gpu, 0x40032CU, 0);
    nv50_wr32(gpu, 0x400330U, 0);
    nv50_wr32(gpu, 0x402CC0U, 0); // GT215's zcull setup
    nv50_wr32(gpu, 0x402CA8U, 0x00000002U);
    for (uint32_t region = 0; region < 8U; region++)
        for (uint32_t word = 0; word < 4U; word++)
            nv50_wr32(gpu, 0x402C20U + region * 0x10U + word * 4U, 0);

    uint32_t context = nv50_gr_context_new(gpu);
    if (!context) return -1;
    volatile uint32_t *inst = gpu->fifo.inst_map;
    nv50_gr_context_bind(inst, context);
    // The 2D object, as nv50_gr_object_bind writes one.
    inst[(INST_GR_2D + 0x00U) / 4U] = CLASS_2D;
    inst[(INST_GR_2D + 0x04U) / 4U] = 0;
    inst[(INST_GR_2D + 0x08U) / 4U] = 0;
    inst[(INST_GR_2D + 0x0CU) / 4U] = 0;
    nv50_fifo_ramht_add(gpu, HANDLE_2D, (ENGINE_GR << 20) | (INST_GR_2D >> 4));
    (void)inst[0];
    if (nv50_bar_flush(gpu) != 0) return -1;
    kprintf("NV50: gr: units %x, %s, ctxprog loaded, %u KiB context at %x\n", units,
            ram.type == NVKM_RAM_TYPE_GDDR5 ? "gddr5" : "not gddr5", context_bytes >> 10, context);
    return 0;
}

// PGRAPH waits once it raises an interrupt -- a notify, an illegal method, a
// trap -- until the interrupt is acknowledged. Its interrupts are off here,
// so the vblank interrupt looks every frame and this answers them as
// nv50_gr_intr does: reported, traps reset as nv50_gr_init sets them, and
// PGRAPH let go on.
int nv50_gr_pending(const struct nv50_device *gpu) {
    return context_bytes && nv50_rd32(gpu, GR_INTR) != 0;
}

void nv50_gr_recover(struct nv50_device *gpu) {
    uint32_t status = nv50_rd32(gpu, GR_INTR);
    if (!status) return;
    static unsigned reported;
    uint32_t trap = nv50_rd32(gpu, GR_TRAP);
    if (reported < 16U) {
        reported++;
        uint32_t address = nv50_rd32(gpu, 0x400704U);
        kprintf("NV50: PGRAPH interrupt %x: trap %x, subchannel %u method %x data %x, class %x,"
                " channel instance %x\n",
                status, trap, (address >> 16) & 7U, address & 0x1FFCU, nv50_rd32(gpu, 0x400708U),
                nv50_rd32(gpu, 0x400814U), (nv50_rd32(gpu, 0x40032CU) & 0x0FFFFFFFU) << 12);
    }
    if (trap) {
        static const uint32_t units[] = {0x400804U, 0x406800U, 0x400C04U,
                                         0x401800U, 0x405018U, 0x402000U};
        for (unsigned index = 0; index < sizeof(units) / sizeof(units[0]); index++)
            nv50_wr32(gpu, units[index], 0xC0000000U);
        uint32_t tps = nv50_rd32(gpu, 0x001540U);
        for (uint32_t unit = 0; unit < 16U; unit++) {
            if (!(tps & (1U << unit))) continue;
            nv50_wr32(gpu, 0x408600U + (unit << 11), 0xC0000000U);
            nv50_wr32(gpu, 0x408708U + (unit << 11), 0xC0000000U);
            nv50_wr32(gpu, 0x40831CU + (unit << 11), 0xC0000000U);
        }
        nv50_wr32(gpu, GR_TRAP, trap);
    }
    nv50_wr32(gpu, GR_INTR, status);
    nv50_wr32(gpu, 0x400500U, 0x00010001U);
}

static void report_gr(const struct nv50_device *gpu, const char *what) {
    kprintf("NV50: gr %s: intr %x, trap %x, status %x, vstatus %x %x %x, ctxctl %x\n", what,
            nv50_rd32(gpu, GR_INTR), nv50_rd32(gpu, GR_TRAP), nv50_rd32(gpu, GR_STATUS),
            nv50_rd32(gpu, 0x400380U), nv50_rd32(gpu, 0x400384U), nv50_rd32(gpu, 0x400388U),
            nv50_rd32(gpu, 0x400304U));
}

// The 2D engine fills a rectangle of a VRAM surface: PGRAPH's context
// switched in and the first thing it draws.
#define FILL_WIDTH  256U
#define FILL_HEIGHT 256U
#define FILL_PITCH  (FILL_WIDTH * 4U)
#define FILL_COLOUR 0xFF336699U

void nv50_gr_selftest(struct nv50_device *gpu) {
    uint32_t surface = nv50_vram_alloc(&gpu->vram, FILL_PITCH * FILL_HEIGHT, 0x10000U);
    volatile uint32_t *pixels = surface ? nv50_vram_map(gpu, surface, FILL_PITCH * FILL_HEIGHT) : NULL;
    if (!pixels) return;
    for (uint32_t word = 0; word < FILL_WIDTH * FILL_HEIGHT; word++) pixels[word] = 0;
    (void)pixels[0];
    (void)nv50_bar_flush(gpu);

    uint32_t start = nv50_fifo_begin(gpu, 64U);
    uint32_t object = HANDLE_2D;
    uint32_t dma[3] = {0xF0000010U, 0xF0000010U, 0xF0000010U}; // the VM DMA object
    uint32_t dst[2] = {0xCFU, 1U};                             // A8R8G8B8, pitch
    uint32_t size[5] = {FILL_PITCH, FILL_WIDTH, FILL_HEIGHT, 0, surface};
    uint32_t clip = 0, operation = 3U;                         // no clip, SRCCOPY
    uint32_t solid[3] = {4U, 0xCFU, FILL_COLOUR};              // rectangles, A8R8G8B8, colour
    // Corners (16,16) and (240,240): the rest stays zero.
    uint32_t corners[4] = {16U, 16U, 240U, 240U};
    nv50_fifo_method(gpu, SUBC_2D, 0x0000U, 1, &object);
    nv50_fifo_method(gpu, SUBC_2D, 0x0184U, 3, dma);
    nv50_fifo_method(gpu, SUBC_2D, 0x0200U, 2, dst);
    nv50_fifo_method(gpu, SUBC_2D, 0x0214U, 5, size);
    nv50_fifo_method(gpu, SUBC_2D, 0x0290U, 1, &clip);
    nv50_fifo_method(gpu, SUBC_2D, 0x02ACU, 1, &operation);
    nv50_fifo_method(gpu, SUBC_2D, 0x0580U, 3, solid);
    nv50_fifo_method(gpu, SUBC_2D, 0x0600U, 4, corners);
    if (nv50_fifo_kick(gpu, start) != 0) return;
    uint64_t started = time_uptime_ns();
    uint32_t sequence = nv50_fifo_fence(gpu);
    int fenced = sequence && nv50_fifo_wait(gpu, sequence, 1000000000ULL) == 0;
    uint64_t took = time_uptime_ns() - started;

    unsigned inside = 0, outside = 0, wrong_in = 0, wrong_out = 0;
    for (uint32_t y = 0; y < FILL_HEIGHT; y++)
        for (uint32_t x = 0; x < FILL_WIDTH; x++) {
            int in = x >= 16U && x < 240U && y >= 16U && y < 240U;
            uint32_t value = pixels[y * FILL_WIDTH + x];
            if (in) inside++, wrong_in += value != FILL_COLOUR;
            else outside++, wrong_out += value != 0;
        }
    kprintf("NV50: gr: 2d fill %s after %u us: %u of %u pixels inside wrong, %u of %u outside, "
            "first %x\n",
            fenced ? "fenced" : "not fenced", (unsigned)(took / 1000ULL), wrong_in, inside, wrong_out,
            outside, pixels[16U * FILL_WIDTH + 16U]);
    if (!fenced || wrong_in || wrong_out) report_gr(gpu, "after the fill");
}

// A block-linear surface of WIDTH x HEIGHT 32-bit pixels at SRC (mapped with
// its kind), its block height TILE_MODE as GEM keeps it, copied into the
// pitch-linear DST, as nv50_2d_texture_set and nv50_blit_2d set up the 2D
// engine; the fence of it, 0 when it could not be queued.
uint32_t nv50_gr_blit(struct nv50_device *gpu, uint32_t dst, uint32_t dst_pitch, uint64_t src,
                      uint32_t tile_mode, uint32_t width, uint32_t height) {
    if (!context_bytes || !width || !height) return 0;
    uint32_t start = nv50_fifo_begin(gpu, 64U);
    uint32_t object = HANDLE_2D;
    uint32_t dma[2] = {0xF0000010U, 0xF0000010U};
    uint32_t dst_format[2] = {0xCFU, 1U};
    uint32_t dst_surface[5] = {dst_pitch, width, height, 0, dst};
    uint32_t src_format[5] = {0xCFU, 0, tile_mode << 4, 1U, 0};
    uint32_t src_surface[4] = {width, height, (uint32_t)(src >> 32), (uint32_t)src};
    uint32_t clip = 0, operation = 3U, control = 0;
    uint32_t rectangle[4] = {0, 0, width, height};
    uint32_t scale[4] = {0, 1U, 0, 1U};
    uint32_t origin[4] = {0, 0, 0, 0};
    nv50_fifo_method(gpu, SUBC_2D, 0x0000U, 1, &object);
    nv50_fifo_method(gpu, SUBC_2D, 0x0184U, 2, dma);
    nv50_fifo_method(gpu, SUBC_2D, 0x0200U, 2, dst_format);
    nv50_fifo_method(gpu, SUBC_2D, 0x0214U, 5, dst_surface);
    nv50_fifo_method(gpu, SUBC_2D, 0x0230U, 5, src_format);
    nv50_fifo_method(gpu, SUBC_2D, 0x0248U, 4, src_surface);
    nv50_fifo_method(gpu, SUBC_2D, 0x0290U, 1, &clip);
    nv50_fifo_method(gpu, SUBC_2D, 0x02ACU, 1, &operation);
    nv50_fifo_method(gpu, SUBC_2D, 0x088CU, 1, &control);
    nv50_fifo_method(gpu, SUBC_2D, 0x08B0U, 4, rectangle);
    nv50_fifo_method(gpu, SUBC_2D, 0x08C0U, 4, scale);
    nv50_fifo_method(gpu, SUBC_2D, 0x08D0U, 4, origin);
    if (nv50_fifo_kick(gpu, start) != 0) return 0;
    return nv50_fifo_fence(gpu);
}
