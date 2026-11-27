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
#define VRAM_CURSOR         0x00830000U
#define VRAM_LUT            0x00834000U
#define VRAM_SURFACE        0x01000000U

// DCB output types and the connectors scripts and probing care about.
#define NV50_OUTPUT_CRT  0U
#define NV50_OUTPUT_TV   1U
#define NV50_OUTPUT_TMDS 2U
#define NV50_OUTPUT_LVDS 3U
#define NV50_OUTPUT_DP   6U

#define NV50_CONNECTOR_VGA    0x00U
#define NV50_CONNECTOR_LVDS   0x40U
#define NV50_CONNECTOR_DP     0x46U
#define NV50_CONNECTOR_EDP    0x47U
#define NV50_CONNECTOR_HDMI   0x61U

#define NV50_OUTPUTS   16U
#define NV50_I2C_PORTS 16U
#define NV50_UNUSED    0xFFU

// An output of the DCB: a DAC or SOR wired to a connector, and the i2c port
// its monitor answers on.
struct nv50_output {
    uint8_t type;
    uint8_t i2c;
    uint8_t heads;
    uint8_t connector;
    uint8_t location;
    uint8_t or_mask;
    uint8_t or_index;
    uint8_t link;
    uint8_t connector_type;
    uint8_t hpd;
    uint16_t hash_type;
    uint16_t hash_mask;
    int edid_ok;
    uint8_t edid[128];
};

// An i2c port of the DCB: a bit-banged bus ('drive') and, for one whose pins
// it shares with a DisplayPort aux channel, the pad to switch ('share').
struct nv50_i2c_port {
    uint8_t type;
    uint8_t drive;
    uint8_t share;
};

#define NV50_INTR_VBLANK     1
#define NV50_INTR_SUPERVISOR 2

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

#define NV50_VRAM_RANGES 32U

struct nv50_vram_range {
    uint32_t start;
    uint32_t end;
};

struct nv50_vram {
    struct nv50_vram_range ranges[NV50_VRAM_RANGES];
    unsigned count;
};

struct nv50_fifo {
    uint32_t inst;
    uint32_t runlist;
    uint32_t ring;
    uint32_t sema;
    uint32_t pt[2];
    volatile uint32_t *ring_map;
    volatile uint32_t *sema_map;
    volatile uint32_t *inst_map;
    uint32_t gart_pt;
    volatile uint32_t *gart_map;
    struct nv50_vram gart;
    int copy_ready;
    uint32_t put;
    uint32_t ib_put;
    uint32_t sequence;
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
    uint16_t init_table;
    uint16_t init_table_bytes;
    struct nv50_output outputs[NV50_OUTPUTS];
    unsigned output_count;
    struct nv50_i2c_port i2c[NV50_I2C_PORTS];
    unsigned i2c_count;
    int lvds_output;
    volatile int supervisor_interrupt;
    unsigned channel_errors;
    struct nv50_vram vram;
    struct nv50_fifo fifo;
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

int nv50_vram_init(struct nv50_vram *vram, uint32_t start, uint32_t end);
uint32_t nv50_vram_alloc(struct nv50_vram *vram, uint32_t bytes, uint32_t align);
void nv50_vram_free(struct nv50_vram *vram, uint32_t at, uint32_t bytes);
uint32_t nv50_vram_free_bytes(const struct nv50_vram *vram);
int nv50_fifo_start(struct nv50_device *gpu);
uint32_t nv50_fifo_fence(struct nv50_device *gpu);
int nv50_fifo_wait(struct nv50_device *gpu, uint32_t sequence, uint64_t timeout_ns);
void nv50_fifo_selftest(struct nv50_device *gpu);
int nv50_copy(struct nv50_device *gpu, uint32_t dst, uint32_t src, uint32_t bytes);
int nv50_copy_start(struct nv50_device *gpu);
uint32_t nv50_copy_rect(struct nv50_device *gpu, uint64_t dst, uint32_t dst_pitch, uint64_t src,
                        uint32_t src_pitch, uint32_t line_bytes, uint32_t lines);
int nv50_fifo_done(const struct nv50_device *gpu, uint32_t sequence);
uint64_t nv50_gart_map(struct nv50_device *gpu, const uint64_t *pages, uint32_t count);
void nv50_gart_unmap(struct nv50_device *gpu, uint64_t address, uint32_t count);
uint32_t nv50_fifo_begin(struct nv50_device *gpu, uint32_t words);
void nv50_fifo_method(struct nv50_device *gpu, unsigned subchannel, uint32_t method, unsigned count,
                      const uint32_t *data);
int nv50_fifo_kick(struct nv50_device *gpu, uint32_t start);
void nv50_fifo_ramht_add(struct nv50_device *gpu, uint32_t handle, uint32_t context);
volatile uint32_t *nv50_vram_map(const struct nv50_device *gpu, uint32_t at, uint32_t bytes);
int nv50_bar_flush(const struct nv50_device *gpu);
int nv50_gr_start(struct nv50_device *gpu);
void nv50_gr_selftest(struct nv50_device *gpu);

int nv50_bios_read(struct nv50_device *gpu, uint8_t *image, uint32_t capacity);
uint16_t nv50_bit_entry(const struct nv50_device *gpu, uint8_t id, uint8_t *version,
                        uint16_t *length);
uint8_t nv50_bios8(const struct nv50_device *gpu, uint32_t at);
uint16_t nv50_bios16(const struct nv50_device *gpu, uint32_t at);
uint32_t nv50_bios32(const struct nv50_device *gpu, uint32_t at);
int nv50_bios_find_lvds(struct nv50_device *gpu, uint32_t khz);
int nv50_bios_run(struct nv50_device *gpu, uint16_t script);

int nv50_dcb_parse(struct nv50_device *gpu);
const char *nv50_output_name(const struct nv50_output *outp);

int nv50_init_run(struct nv50_device *gpu, uint16_t script, const struct nv50_output *outp,
                  unsigned head);
int nv50_init_check(struct nv50_device *gpu, uint16_t script, const struct nv50_output *outp);
void nv50_check_scripts(struct nv50_device *gpu);

int nv50_i2c_read(struct nv50_device *gpu, unsigned port, uint8_t address, uint8_t offset,
                  uint8_t *data, unsigned bytes);
void nv50_probe_outputs(struct nv50_device *gpu);

int nv50_disp_init(struct nv50_device *gpu);
int nv50_disp_modeset(struct nv50_device *gpu);
int nv50_disp_flip(struct nv50_device *gpu, uint32_t vram);
int nv50_disp_flip_idle(struct nv50_device *gpu, int may_service);
void nv50_disp_vblank_start(struct nv50_device *gpu);
void nv50_disp_vblank_stop(struct nv50_device *gpu);
int nv50_disp_interrupt(struct nv50_device *gpu, uint32_t *supervisor);
void nv50_disp_supervise(struct nv50_device *gpu, uint32_t pending);
void nv50_disp_msi_rearm(struct nv50_device *gpu);
int nv50_disp_cursor_start(struct nv50_device *gpu);
int nv50_disp_cursor_show(struct nv50_device *gpu, uint32_t vram);
int nv50_disp_cursor_move(struct nv50_device *gpu, int32_t x, int32_t y);
int nv50_disp_gamma(struct nv50_device *gpu, uint32_t vram);

int nv50_disp_backlight_present(const struct nv50_device *gpu);
uint32_t nv50_disp_backlight_get(const struct nv50_device *gpu, uint32_t scale);
int nv50_disp_backlight_set(const struct nv50_device *gpu, uint32_t level, uint32_t scale);

#endif
