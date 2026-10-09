#include <stddef.h>
#include <stdint.h>

#include <tunix/cpu.h>
#include <tunix/boot.h>
#include <tunix/boot_framebuffer.h>
#include <tunix/nv50.h>
#include <tunix/pci.h>
#include <tunix/time.h>
#include <tunix/vmm.h>

#define PMC_INTR            0x000100U
#define PMC_INTR_EN         0x000140U
#define PBUS_PRAMIN_BASE    0x001700U
#define PBUS_BAR1_VM        0x001704U
#define PBUS_FLUSH          0x070000U
#define PRAMIN_WINDOW       0x700000U
#define PRAMIN_WINDOW_BYTES 0x100000U
#define PDISP_OBJECTS       0x610010U
#define PDISP_INTR_0        0x610020U
#define PDISP_INTR_1        0x610024U
#define PDISP_INTR_EN_0     0x610028U
#define PDISP_INTR_EN_1     0x61002CU
#define PDISP_SUPERVISOR    0x610030U
#define PDISP_ERROR_ADDR    0x610080U
#define PDISP_ERROR_DATA    0x610084U
#define PDISP_CAP           0x610184U
#define PDISP_CORE_CTRL     0x610200U
#define PDISP_CORE_PUSH     0x610204U
#define PDISP_CORE_UNK08    0x610208U
#define PDISP_CORE_UNK0C    0x61020CU
#define PDISP_VGA_OWNER     0x6194E8U
#define PDISP_CORE_PUT      0x640000U
#define PDISP_CORE_GET      0x640004U
#define PDISP_VBIOS_OWNS    0x00000100U
#define PDISP_VBIOS_POINTER 0x619F04U
#define PCI_VENDOR_NVIDIA   0x10DEU
#define NV_BAR0_BYTES       0x1000000ULL

#define VRAM_INSTANCE       0x00800000U
#define VRAM_INSTANCE_BYTES 0x10000U
#define VRAM_PUSH           0x00810000U
#define VRAM_SYNC           0x00811000U
#define VRAM_TEST           0x00820000U
#define RAMHT_BYTES         0x1000U
#define RAMHT_BITS          9U
#define OBJECT_VRAM         0x1000U
#define OBJECT_SYNC         0x1020U
#define HANDLE_SYNC         0xF0000000U
#define HANDLE_VRAM         0xF0000001U
#define DMA_IN_MEMORY       0x3DU
#define DMA_TARGET_VRAM     0x00010000U
#define DMA_ACCESS_RW       0x00080000U
#define DMA_PRIV_US         (1U << 20)
#define DMA_PART_256        (1U << 16)

#define CORE_UPDATE                 0x0080U
#define CORE_SET_NOTIFIER_CONTROL   0x0084U
#define CORE_SET_CONTEXT_DMA_NOTIFY 0x0088U
#define CORE_GET_CAPABILITIES       0x008CU
#define NOTIFY_ENABLE               0x80000000U
#define TIMEOUT_NS                  2000000000ULL
#define SUPERVISOR_TIMEOUT_NS       5000000000ULL

#define HEAD_SET_OFFSET            0x0860U
#define HEAD_SET_SIZE              0x0868U
#define HEAD_SET_STORAGE           0x086CU
#define HEAD_SET_PARAMS            0x0870U
#define HEAD_SET_CONTEXT_DMA_ISO   0x0874U
#define HEAD_SET_VIEWPORT_POINT_IN 0x08C0U
#define HEAD_SET_VIEWPORT_SIZE_IN  0x08C8U
#define HEAD_SET_VIEWPORT_SIZE_OUT 0x08D8U
#define HEAD_SET_VIEWPORT_OUT_MIN  0x08DCU
#define HEAD_ARMED_SIZE_OUT        0x610B4CU
#define SOR_ARMED_CONTROL          0x610798U
#define STORAGE_PITCH_LAYOUT       0x00100000U
#define FORMAT_A8R8G8B8            0xCFU

#define VRAM_SURFACE        0x01000000U
#define SURFACE_PITCH_ALIGN 256U

#define OP_ZM_REG_SEQUENCE 0x58U
#define OP_SUB_DIRECT      0x5BU
#define OP_NV_REG          0x6EU
#define OP_DONE            0x71U
#define OP_TIME            0x74U
#define SCRIPT_MAX_STEPS   512U
#define SCRIPT_MAX_DEPTH   8U
#define PUSH_USABLE_BYTES  0xFC0U

struct probe {
    uint64_t bar0;
    uint64_t bar1;
    uint32_t vram_bytes;
    uint32_t pramin_saved;
    const uint8_t *vbios;
    uint32_t vbios_bytes;
    uint32_t surface_width;
    uint32_t surface_height;
    uint32_t surface_pitch;
    unsigned script_steps;
    char *log;
    size_t used;
    size_t capacity;
};

struct lvds_output {
    uint32_t vpll_coefficients;
    uint32_t vpll_fraction;
    uint16_t hash_type;
    uint16_t hash_mask;
    uint8_t or_index;
    uint8_t link;
    uint16_t scripts[3];
    uint16_t on_int2;
    uint16_t on_int3;
};

static void say(struct probe *probe, const char *text) {
    while (*text && probe->used + 1 < probe->capacity) probe->log[probe->used++] = *text++;
    probe->log[probe->used] = '\0';
}

static void say_hex(struct probe *probe, uint64_t value, unsigned digits) {
    static const char alphabet[] = "0123456789abcdef";
    char out[20];
    unsigned count = 0;
    out[count++] = '0';
    out[count++] = 'x';
    while (digits--) out[count++] = alphabet[(value >> (digits * 4U)) & 0xFU];
    out[count] = '\0';
    say(probe, out);
}

static void say_reg(struct probe *probe, const char *name, uint32_t offset, uint32_t value) {
    say(probe, "  ");
    say(probe, name);
    say(probe, " [");
    say_hex(probe, offset, 6);
    say(probe, "] = ");
    say_hex(probe, value, 8);
    say(probe, "\n");
}

static uint32_t rd32(struct probe *probe, uint32_t offset) {
    return *(volatile const uint32_t *)(probe->bar0 + offset);
}

static void wr32(struct probe *probe, uint32_t offset, uint32_t value) {
    *(volatile uint32_t *)(probe->bar0 + offset) = value;
}

static void mask32(struct probe *probe, uint32_t offset, uint32_t mask, uint32_t value) {
    wr32(probe, offset, (rd32(probe, offset) & ~mask) | value);
}

static void show(struct probe *probe, const char *name, uint32_t offset) {
    say_reg(probe, name, offset, rd32(probe, offset));
}

static int wait_clear(struct probe *probe, uint32_t offset, uint32_t mask) {
    uint64_t deadline = time_uptime_ns() + TIMEOUT_NS;
    while (rd32(probe, offset) & mask) {
        if (time_uptime_ns() > deadline) return -1;
        cpu_relax();
    }
    return 0;
}

static void pramin_select(struct probe *probe, uint32_t vram) {
    wr32(probe, PBUS_PRAMIN_BASE, vram >> 16);
}

static uint32_t vram_rd32(struct probe *probe, uint32_t vram) {
    return rd32(probe, PRAMIN_WINDOW + (vram - VRAM_INSTANCE));
}

static void vram_wr32(struct probe *probe, uint32_t vram, uint32_t value) {
    wr32(probe, PRAMIN_WINDOW + (vram - VRAM_INSTANCE), value);
}

static int vram_flush(struct probe *probe) {
    wr32(probe, PBUS_FLUSH, 0x00000001U);
    return wait_clear(probe, PBUS_FLUSH, 0x00000002U);
}

static void state(struct probe *probe, const char *when) {
    say(probe, when);
    say(probe, "\n");
    show(probe, "pmc intr       ", PMC_INTR);
    show(probe, "pmc intr en    ", PMC_INTR_EN);
    show(probe, "pramin base    ", PBUS_PRAMIN_BASE);
    show(probe, "bar1 vm        ", PBUS_BAR1_VM);
    show(probe, "disp objects   ", PDISP_OBJECTS);
    show(probe, "disp intr 0    ", PDISP_INTR_0);
    show(probe, "disp intr 1    ", PDISP_INTR_1);
    show(probe, "disp intr en 0 ", PDISP_INTR_EN_0);
    show(probe, "disp intr en 1 ", PDISP_INTR_EN_1);
    show(probe, "supervisor     ", PDISP_SUPERVISOR);
    show(probe, "core ctrl      ", PDISP_CORE_CTRL);
    show(probe, "core push      ", PDISP_CORE_PUSH);
    show(probe, "core 208       ", PDISP_CORE_UNK08);
    show(probe, "core 20c       ", PDISP_CORE_UNK0C);
    show(probe, "vga owner      ", PDISP_VGA_OWNER);
    show(probe, "core put       ", PDISP_CORE_PUT);
    show(probe, "core get       ", PDISP_CORE_GET);
    show(probe, "core error adr ", PDISP_ERROR_ADDR);
    show(probe, "core error data", PDISP_ERROR_DATA);
}

static int test_vram(struct probe *probe) {
    say(probe, "vram access\n");
    static const uint32_t pattern[4] = {0x54554E49U, 0x58000001U, 0xA5A55A5AU, 0x0BADF00DU};
    for (unsigned index = 0; index < 4; index++)
        vram_wr32(probe, VRAM_TEST + index * 4U, pattern[index]);
    if (vram_flush(probe) != 0) say(probe, "  flush did not finish\n");
    int pramin_ok = 1;
    for (unsigned index = 0; index < 4; index++)
        pramin_ok &= vram_rd32(probe, VRAM_TEST + index * 4U) == pattern[index];
    say(probe, pramin_ok ? "  pramin read back ok\n" : "  pramin read back WRONG\n");
    if (!pramin_ok) return -1;

    if (!probe->bar1) {
        say(probe, "  bar1 not assigned\n");
        return 0;
    }
    uint64_t bar1 = vmm_map_device(probe->bar1 + VRAM_TEST, 0x1000U);
    if (!bar1) {
        say(probe, "  bar1 not mapped\n");
        return 0;
    }
    volatile uint32_t *linear = (volatile uint32_t *)bar1;
    int bar1_sees = 1;
    for (unsigned index = 0; index < 4; index++) bar1_sees &= linear[index] == pattern[index];
    linear[4] = 0x0D15EA5EU;
    (void)linear[4];
    int pramin_sees = vram_rd32(probe, VRAM_TEST + 16U) == 0x0D15EA5EU;
    say(probe,
        bar1_sees ? "  bar1 reads what pramin wrote: linear\n"
                  : "  bar1 does not read what pramin wrote\n");
    say(probe,
        pramin_sees ? "  pramin reads what bar1 wrote\n"
                    : "  pramin does not read what bar1 wrote\n");
    say_reg(probe, "bar1 word 0    ", VRAM_TEST, linear[0]);
    return 0;
}

static uint32_t ramht_hash(uint32_t handle, uint32_t channel) {
    uint32_t hash = 0;
    while (handle) {
        hash ^= handle & ((1U << RAMHT_BITS) - 1U);
        handle >>= RAMHT_BITS;
    }
    return hash ^ (channel << (RAMHT_BITS - 4U));
}

static void write_ctxdma(struct probe *probe, uint32_t object, uint32_t start, uint32_t limit) {
    uint32_t base = VRAM_INSTANCE + object;
    vram_wr32(probe, base + 0x00U, DMA_PRIV_US | DMA_ACCESS_RW | DMA_TARGET_VRAM | DMA_IN_MEMORY);
    vram_wr32(probe, base + 0x04U, limit);
    vram_wr32(probe, base + 0x08U, start);
    vram_wr32(probe, base + 0x0CU, 0);
    vram_wr32(probe, base + 0x10U, 0);
    vram_wr32(probe, base + 0x14U, DMA_PART_256);
}

static void bind_object(struct probe *probe, uint32_t handle, uint32_t object) {
    uint32_t slot = ramht_hash(handle, 0);
    while (vram_rd32(probe, VRAM_INSTANCE + slot * 8U)) slot = (slot + 1U) % (RAMHT_BYTES / 8U);
    vram_wr32(probe, VRAM_INSTANCE + slot * 8U, handle);
    vram_wr32(probe, VRAM_INSTANCE + slot * 8U + 4U, object << 10);
    say(probe, "  handle ");
    say_hex(probe, handle, 8);
    say(probe, " -> ramht slot ");
    say_hex(probe, slot, 3);
    say(probe, " object ");
    say_hex(probe, object, 4);
    say(probe, "\n");
}

static int build_objects(struct probe *probe) {
    say(probe, "display objects\n");
    for (uint32_t offset = 0; offset < VRAM_INSTANCE_BYTES; offset += 4U)
        vram_wr32(probe, VRAM_INSTANCE + offset, 0);
    for (uint32_t offset = 0; offset < 0x2000U; offset += 4U)
        vram_wr32(probe, VRAM_PUSH + offset, 0);
    write_ctxdma(probe, OBJECT_VRAM, 0, probe->vram_bytes - 1U);
    write_ctxdma(probe, OBJECT_SYNC, VRAM_SYNC, VRAM_SYNC + 0xFFFU);
    bind_object(probe, HANDLE_VRAM, OBJECT_VRAM);
    bind_object(probe, HANDLE_SYNC, OBJECT_SYNC);
    if (vram_flush(probe) != 0) {
        say(probe, "  flush did not finish\n");
        return -1;
    }
    int ok = vram_rd32(probe, VRAM_INSTANCE + OBJECT_SYNC + 0x08U) == VRAM_SYNC;
    say(probe, ok ? "  objects read back ok\n" : "  objects read back WRONG\n");
    return ok ? 0 : -1;
}

static void take_over(struct probe *probe) {
    say(probe, "take over from the vbios\n");
    wr32(probe, PDISP_CAP, rd32(probe, 0x614004U));
    for (uint32_t head = 0; head < 2; head++)
        for (uint32_t word = 0; word < 4; word++)
            wr32(probe, 0x610190U + head * 0x10U + word * 4U,
                 rd32(probe, 0x616100U + head * 0x800U + word * 4U));
    uint32_t cap = rd32(probe, PDISP_CAP);
    for (uint32_t index = 0; index < 3; index++)
        if (cap & (0x00100000U << index))
            wr32(probe, 0x6101D0U + index * 4U, rd32(probe, 0x61A000U + index * 0x800U));
    for (uint32_t index = 0; index < 4; index++)
        if (cap & (0x01000000U << index))
            wr32(probe, 0x6101E0U + index * 4U, rd32(probe, 0x61C000U + index * 0x800U));
    for (uint32_t index = 0; index < 3; index++)
        if (cap & (0x10000000U << index))
            wr32(probe, 0x6101F0U + index * 4U, rd32(probe, 0x61E000U + index * 0x800U));
    say_reg(probe, "caps           ", PDISP_CAP, cap);

    if (rd32(probe, PDISP_INTR_1) & PDISP_VBIOS_OWNS) {
        wr32(probe, PDISP_INTR_1, PDISP_VBIOS_OWNS);
        mask32(probe, PDISP_VGA_OWNER, 0x00000001U, 0);
        say(probe,
            wait_clear(probe, PDISP_VGA_OWNER, 0x00000002U) == 0
                ? "  vbios let go of the display\n"
                : "  vbios did NOT let go of the display\n");
    } else {
        say(probe, "  the vbios did not own the display\n");
    }
    show(probe, "vga owner      ", PDISP_VGA_OWNER);
    wr32(probe, PDISP_OBJECTS, (VRAM_INSTANCE >> 8) | 9U);
    show(probe, "disp objects   ", PDISP_OBJECTS);
}

static int start_core(struct probe *probe) {
    say(probe, "core channel\n");
    if ((rd32(probe, PDISP_CORE_CTRL) & 0x009F0000U) == 0x00020000U)
        mask32(probe, PDISP_CORE_CTRL, 0x00800000U, 0x00800000U);
    if ((rd32(probe, PDISP_CORE_CTRL) & 0x003F0000U) == 0x00030000U)
        mask32(probe, PDISP_CORE_CTRL, 0x00600000U, 0x00600000U);
    wr32(probe, PDISP_CORE_PUSH, (VRAM_PUSH >> 8) | 1U);
    wr32(probe, PDISP_CORE_UNK08, 0x00010000U);
    wr32(probe, PDISP_CORE_UNK0C, 0);
    mask32(probe, PDISP_CORE_CTRL, 0x00000010U, 0x00000010U);
    wr32(probe, PDISP_CORE_PUT, 0);
    wr32(probe, PDISP_CORE_CTRL, 0x01000013U);
    int ok = wait_clear(probe, PDISP_CORE_CTRL, 0x80000000U) == 0;
    say(probe, ok ? "  core channel is up\n" : "  core channel did NOT come up\n");
    show(probe, "core ctrl      ", PDISP_CORE_CTRL);
    return ok ? 0 : -1;
}

static uint8_t bios8(struct probe *probe, uint32_t at) {
    return at < probe->vbios_bytes ? probe->vbios[at] : 0;
}

static uint16_t bios16(struct probe *probe, uint32_t at) {
    return (uint16_t)(bios8(probe, at) | (bios8(probe, at + 1U) << 8));
}

static uint32_t bios32(struct probe *probe, uint32_t at) {
    return (uint32_t)bios16(probe, at) | ((uint32_t)bios16(probe, at + 2U) << 16);
}

static uint32_t bit_entry(struct probe *probe, uint8_t id, uint8_t *version) {
    for (uint32_t at = 0; at + 12U < probe->vbios_bytes; at++) {
        if (bios8(probe, at) != 0xFFU || bios8(probe, at + 1U) != 0xB8U ||
            bios8(probe, at + 2U) != 'B' || bios8(probe, at + 3U) != 'I' ||
            bios8(probe, at + 4U) != 'T')
            continue;
        uint8_t size = bios8(probe, at + 9U);
        uint8_t count = bios8(probe, at + 10U);
        uint32_t entry = at + 12U;
        for (uint8_t index = 0; index < count; index++, entry += size) {
            if (bios8(probe, entry) != id) continue;
            *version = bios8(probe, entry + 1U);
            return bios16(probe, entry + 4U);
        }
        return 0;
    }
    return 0;
}

static int find_lvds(struct probe *probe, struct lvds_output *out) {
    uint16_t dcb = bios16(probe, 0x36U);
    if (!dcb || bios8(probe, dcb) < 0x40U || bios32(probe, dcb + 6U) != 0x4EDCBDCBU) {
        say(probe, "  no dcb 4.x table\n");
        return -1;
    }
    uint8_t header = bios8(probe, dcb + 1U);
    uint8_t count = bios8(probe, dcb + 2U);
    uint8_t size = bios8(probe, dcb + 3U);
    for (uint8_t index = 0; index < count; index++) {
        uint32_t entry = dcb + header + (uint32_t)index * size;
        uint32_t connection = bios32(probe, entry);
        uint32_t config = bios32(probe, entry + 4U);
        if ((connection & 0xFU) == 0xEU) break;
        if ((connection & 0xFU) != 3U) continue;
        uint8_t location = (uint8_t)((connection >> 20) & 3U);
        uint8_t or_mask = (uint8_t)((connection >> 24) & 0x1FU);
        out->link = (uint8_t)((config >> 4) & 3U);
        out->hash_type = (uint16_t)((location << 4) | 3U);
        out->or_index = (uint8_t)__builtin_ctz(or_mask ? or_mask : 1U);
        out->hash_mask = (uint16_t)(0x0100U | ((uint32_t)__builtin_ffs(out->link) << 6) | or_mask);
        say_reg(probe, "dcb lvds       ", entry, connection);
        say_reg(probe, "dcb lvds conf  ", entry + 4U, config);
        return 0;
    }
    say(probe, "  no lvds output in the dcb\n");
    return -1;
}

static uint16_t clock_script(struct probe *probe, uint16_t compare, uint32_t khz) {
    for (unsigned step = 0; compare && step < 32U; step++, compare = (uint16_t)(compare + 4U))
        if (khz / 10U >= bios16(probe, compare)) return bios16(probe, compare + 2U);
    return 0;
}

static int find_lvds_scripts(struct probe *probe, struct lvds_output *out, uint32_t khz) {
    uint8_t version = 0;
    uint16_t table_pointer = (uint16_t)bit_entry(probe, 'U', &version);
    uint16_t table = table_pointer ? bios16(probe, table_pointer) : 0;
    uint8_t table_version = bios8(probe, table);
    if (!table || version != 1U || table_version < 0x20U || table_version > 0x22U) {
        say(probe, "  no display script table\n");
        return -1;
    }
    uint8_t header = bios8(probe, table + 1U);
    uint8_t size = bios8(probe, table + 2U);
    uint8_t count = bios8(probe, table + 3U);
    uint8_t sub_header = bios8(probe, table + 4U);
    for (uint8_t index = 0; index < count; index++) {
        uint16_t outp = bios16(probe, table + header + (uint32_t)index * size);
        if (!outp || sub_header < 0x0AU) continue;
        uint32_t mask = bios32(probe, outp + 2U);
        if (table_version <= 0x20U) mask |= 0xC0U;
        if (bios16(probe, outp) != out->hash_type || (mask & out->hash_mask) != out->hash_mask)
            continue;
        out->scripts[0] = bios16(probe, outp + 6U);
        out->scripts[1] = bios16(probe, outp + 8U);
        out->scripts[2] = sub_header >= 0x0CU ? bios16(probe, outp + 0x0AU) : 0;
        uint8_t configs = bios8(probe, outp + 5U);
        for (uint8_t config = 0; config < configs; config++) {
            uint32_t at = outp + sub_header + (uint32_t)config * 6U;
            uint8_t proto = bios8(probe, at);
            uint8_t flags = bios8(probe, at + 1U);
            if ((proto != 0U && proto != 0xFFU) || flags != 0U) continue;
            out->on_int2 = clock_script(probe, bios16(probe, at + 2U), khz);
            out->on_int3 = clock_script(probe, bios16(probe, at + 4U), khz);
            say_reg(probe, "ied subtable   ", outp, mask);
            say_reg(probe, "on int2 script ", at + 2U, out->on_int2);
            say_reg(probe, "on int3 script ", at + 4U, out->on_int3);
            say_reg(probe, "off int1 script", outp + 8U, out->scripts[1]);
            say_reg(probe, "off int2 script", outp + 0x0AU, out->scripts[2]);
            return 0;
        }
    }
    say(probe, "  no ied entry matches the lvds output\n");
    return -1;
}

static uint32_t script_register(const struct lvds_output *out, uint32_t reg) {
    reg &= ~3U;
    reg &= ~0x80000000U;
    if (reg & 0x40000000U) {
        reg = (reg & ~0x40000000U) + out->or_index * 0x800U;
        if (reg & 0x20000000U) reg = (reg & ~0x20000000U) + (out->link == 2U) * 0x80U;
    }
    return reg;
}

static int run_script(struct probe *probe, const struct lvds_output *out, uint16_t at,
                      unsigned depth) {
    if (!at) return 0;
    if (depth > SCRIPT_MAX_DEPTH) return -1;
    for (;;) {
        if (++probe->script_steps > SCRIPT_MAX_STEPS) {
            say(probe, "    script too long\n");
            return -1;
        }
        uint8_t op = bios8(probe, at);
        if (op == OP_DONE) return 0;
        if (op == OP_SUB_DIRECT) {
            if (run_script(probe, out, bios16(probe, at + 1U), depth + 1U) != 0) return -1;
            at = (uint16_t)(at + 3U);
        } else if (op == OP_ZM_REG_SEQUENCE) {
            uint32_t reg = bios32(probe, at + 1U);
            uint8_t count = bios8(probe, at + 5U);
            for (uint8_t index = 0; index < count; index++) {
                uint32_t target = script_register(out, reg + index * 4U);
                uint32_t value = bios32(probe, at + 6U + index * 4U);
                if (target > 0xFFFFFCU) return -1;
                wr32(probe, target, value);
                say_reg(probe, "    write      ", target, value);
            }
            at = (uint16_t)(at + 6U + count * 4U);
        } else if (op == OP_NV_REG) {
            uint32_t target = script_register(out, bios32(probe, at + 1U));
            uint32_t keep = bios32(probe, at + 5U);
            uint32_t value = bios32(probe, at + 9U);
            if (target > 0xFFFFFCU) return -1;
            uint32_t now = (rd32(probe, target) & keep) | value;
            wr32(probe, target, now);
            say_reg(probe, "    mask       ", target, now);
            at = (uint16_t)(at + 13U);
        } else if (op == OP_TIME) {
            uint64_t until = time_uptime_ns() + (uint64_t)bios16(probe, at + 1U) * 1000ULL;
            while (time_uptime_ns() < until) cpu_relax();
            at = (uint16_t)(at + 3U);
        } else {
            say_reg(probe, "    UNKNOWN op ", at, op);
            return -1;
        }
    }
}

static void run_named(struct probe *probe, const struct lvds_output *out, const char *name,
                      uint16_t at) {
    say(probe, "  script ");
    say(probe, name);
    say(probe, " ");
    say_hex(probe, at, 4);
    say(probe, "\n");
    if (run_script(probe, out, at, 0) != 0) say(probe, "    script stopped\n");
}

static uint32_t push_at;
static int push_overflow;

static void push(struct probe *probe, uint32_t method, uint32_t data) {
    if (push_at + 8U > PUSH_USABLE_BYTES) {
        push_overflow = 1;
        return;
    }
    vram_wr32(probe, VRAM_PUSH + push_at, (1U << 18) | method);
    vram_wr32(probe, VRAM_PUSH + push_at + 4U, data);
    push_at += 8U;
}

static int kick(struct probe *probe) {
    if (push_overflow) {
        say(probe, "  the push buffer would overflow\n");
        return -1;
    }
    if (vram_flush(probe) != 0) {
        say(probe, "  flush before kick did not finish\n");
        return -1;
    }
    wr32(probe, PDISP_CORE_PUT, push_at);
    uint64_t deadline = time_uptime_ns() + TIMEOUT_NS;
    while (rd32(probe, PDISP_CORE_GET) != push_at) {
        if (time_uptime_ns() > deadline) {
            say(probe, "  the channel did not consume the push buffer\n");
            return -1;
        }
        cpu_relax();
    }
    return 0;
}

static int query_capabilities(struct probe *probe) {
    say(probe, "capabilities\n");
    for (uint32_t offset = 0; offset < 0x100U; offset += 4U)
        vram_wr32(probe, VRAM_SYNC + offset, 0);
    push_at = 0;
    push(probe, CORE_SET_CONTEXT_DMA_NOTIFY, HANDLE_SYNC);
    push(probe, CORE_SET_NOTIFIER_CONTROL, NOTIFY_ENABLE);
    push(probe, CORE_GET_CAPABILITIES, 0);
    push(probe, CORE_SET_NOTIFIER_CONTROL, 0);
    int kicked = kick(probe) == 0;
    say(probe, kicked ? "  push buffer consumed\n" : "");
    uint64_t deadline = time_uptime_ns() + TIMEOUT_NS;
    while (!(vram_rd32(probe, VRAM_SYNC + 4U) & 1U) && time_uptime_ns() < deadline) cpu_relax();
    int done = vram_rd32(probe, VRAM_SYNC + 4U) & 1U;
    say(probe,
        done ? "  capabilities notifier written\n" : "  capabilities notifier NOT written\n");
    for (uint32_t offset = 0; offset < 0x54U; offset += 4U)
        say_reg(probe, "notifier       ", offset, vram_rd32(probe, VRAM_SYNC + offset));
    return kicked && done ? 0 : -1;
}

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

static uint32_t sanitize_armed(uint32_t method, uint32_t value) {
    if (method < 0x0800U || method >= 0x0C00U + 0x400U) return value;
    uint32_t field = (method - 0x0800U) % 0x400U;
    if (field == 0x004U) return value & ~0x02000000U;
    if (field == 0x024U) return 1U;
    return value;
}

static unsigned restore_armed_state(struct probe *probe) {
    unsigned restored = 0;
    for (size_t list = 0; list < sizeof(core_lists) / sizeof(core_lists[0]); list++) {
        const struct method_list *methods = &core_lists[list];
        for (uint8_t instance = 0; instance < methods->count; instance++) {
            for (uint8_t entry = 0; entry < methods->entry_count; entry++) {
                uint32_t reg = methods->entries[entry].reg + instance * methods->reg_stride;
                uint32_t method =
                    methods->entries[entry].method + instance * methods->method_stride;
                uint32_t pending = rd32(probe, reg);
                uint32_t armed = sanitize_armed(method, rd32(probe, reg + 4U));
                if (pending == armed) continue;
                push(probe, method, armed);
                restored++;
                say(probe, "  restore ");
                say_hex(probe, method, 4);
                say(probe, " ");
                say_hex(probe, pending, 8);
                say(probe, " -> ");
                say_hex(probe, armed, 8);
                say(probe, "\n");
            }
        }
    }
    return restored;
}

static void show_error(struct probe *probe) {
    show(probe, "disp intr 0    ", PDISP_INTR_0);
    show(probe, "core error adr ", PDISP_ERROR_ADDR);
    show(probe, "core error data", PDISP_ERROR_DATA);
    for (uint32_t offset = 0x610600U; offset < 0x610640U; offset += 8U) {
        say(probe, "  recent method ");
        say_hex(probe, rd32(probe, offset + 4U), 4);
        say(probe, " data ");
        say_hex(probe, rd32(probe, offset), 8);
        say(probe, "\n");
    }
}

static int service_supervisors(struct probe *probe, const struct lvds_output *out) {
    uint64_t deadline = time_uptime_ns() + SUPERVISOR_TIMEOUT_NS;
    while (time_uptime_ns() < deadline) {
        if (vram_rd32(probe, VRAM_SYNC) & 1U) {
            say(probe, "  update completed\n");
            return 0;
        }
        uint32_t pending = rd32(probe, PDISP_INTR_1) & 0x70U;
        if (!pending) {
            cpu_relax();
            continue;
        }
        uint32_t super = rd32(probe, PDISP_SUPERVISOR);
        say_reg(probe, "supervisor intr", PDISP_INTR_1, pending);
        say_reg(probe, "supervisor     ", PDISP_SUPERVISOR, super);
        wr32(probe, PDISP_INTR_1, pending);
        if (pending & 0x20U) {
            if (super & 0x80U) run_named(probe, out, "off int2", out->scripts[2]);
            if (super & 0x200U) {
                wr32(probe, 0x614104U, out->vpll_coefficients);
                wr32(probe, 0x614108U, out->vpll_fraction);
                say(probe, "  pixel clock pll rewritten with the vbios coefficients\n");
            }
            if (super & 0x80U) {
                run_named(probe, out, "on int2", out->on_int2);
                mask32(probe, 0x614200U, 0x0000000FU, 0);
                mask32(probe, 0x614300U + out->or_index * 0x800U, 0x00000707U, 0);
            }
        } else if (pending & 0x40U) {
            if (super & 0x80U) run_named(probe, out, "on int3", out->on_int3);
        } else if (super & 0xA0U) {
            run_named(probe, out, "off int1", out->scripts[1]);
        }
        wr32(probe, PDISP_SUPERVISOR, 0x80000000U);
    }
    say(probe, "  update did NOT complete\n");
    return -1;
}

static int modeset(struct probe *probe) {
    say(probe, "modeset\n");
    if (!probe->vbios || probe->vbios_bytes < 0x200U || probe->vbios[0] != 0x55U ||
        probe->vbios[1] != 0xAAU) {
        say(probe, "  no video bios image to run scripts from\n");
        return -1;
    }
    uint32_t size_out = rd32(probe, HEAD_ARMED_SIZE_OUT);
    uint32_t width = size_out & 0x7FFFU;
    uint32_t height = (size_out >> 16) & 0x7FFFU;
    uint32_t clock = rd32(probe, 0x610AD4U) & 0x3FFFFFU;
    uint32_t pitch = (width * 4U + SURFACE_PITCH_ALIGN - 1U) & ~(SURFACE_PITCH_ALIGN - 1U);
    say_reg(probe, "panel size     ", HEAD_ARMED_SIZE_OUT, size_out);
    say_reg(probe, "pixel clock khz", 0x610AD4U, clock);
    say_reg(probe, "sor0 control   ", SOR_ARMED_CONTROL, rd32(probe, SOR_ARMED_CONTROL));
    if (width < 640U || height < 480U || !clock) {
        say(probe, "  the vbios mode does not look usable\n");
        return -1;
    }

    struct lvds_output out = {0};
    if (find_lvds(probe, &out) != 0 || find_lvds_scripts(probe, &out, clock) != 0) return -1;

    out.vpll_coefficients = rd32(probe, 0x614104U);
    out.vpll_fraction = rd32(probe, 0x614108U);
    show(probe, "vpll control   ", 0x614100U);
    show(probe, "vpll coeffs    ", 0x614104U);
    show(probe, "vpll fraction  ", 0x614108U);

    probe->surface_width = width;
    probe->surface_height = height;
    probe->surface_pitch = pitch;

    vram_wr32(probe, VRAM_SYNC, 0);
    push(probe, CORE_SET_CONTEXT_DMA_NOTIFY, HANDLE_SYNC);
    say(probe, "  restoring the vbios state the channel reset\n");
    unsigned restored = restore_armed_state(probe);
    say(probe, restored ? "  restored the methods above\n" : "  nothing to restore\n");
    push(probe, HEAD_SET_OFFSET, VRAM_SURFACE >> 8);
    push(probe, HEAD_SET_SIZE, (height << 16) | width);
    push(probe, HEAD_SET_STORAGE, STORAGE_PITCH_LAYOUT | ((pitch >> 8) << 8));
    push(probe, HEAD_SET_PARAMS, FORMAT_A8R8G8B8 << 8);
    push(probe, HEAD_SET_CONTEXT_DMA_ISO, HANDLE_VRAM);
    push(probe, HEAD_SET_VIEWPORT_POINT_IN, 0);
    push(probe, HEAD_SET_VIEWPORT_SIZE_IN, (height << 16) | width);
    push(probe, HEAD_SET_VIEWPORT_SIZE_OUT, (height << 16) | width);
    push(probe, HEAD_SET_VIEWPORT_OUT_MIN, (height << 16) | width);
    push(probe, CORE_SET_NOTIFIER_CONTROL, NOTIFY_ENABLE);
    push(probe, CORE_UPDATE, 0);
    push(probe, CORE_SET_NOTIFIER_CONTROL, 0);
    if (kick(probe) != 0) return -1;
    say(probe, "  update sent\n");
    int result = service_supervisors(probe, &out);
    if (result != 0 || (rd32(probe, PDISP_INTR_0) & 0x001F0000U)) show_error(probe);
    say_reg(probe, "notifier 0     ", VRAM_SYNC, vram_rd32(probe, VRAM_SYNC));
    show(probe, "head offset    ", 0x610A88U);
    show(probe, "head size      ", 0x610B1CU);
    show(probe, "head storage   ", 0x610B24U);
    show(probe, "head params    ", 0x610ACCU);
    show(probe, "head ctxdma    ", 0x610A3CU);
    show(probe, "viewport in    ", 0x610B44U);
    return result;
}

static uint8_t vbios_image[0x20000];
static char log_buffer[32768];
static size_t log_bytes;

static int read_vbios(struct probe *probe) {
    uint32_t pointer = rd32(probe, PDISP_VBIOS_POINTER);
    say_reg(probe, "vbios pointer  ", PDISP_VBIOS_POINTER, pointer);
    if (!(pointer & 0x8U) || (pointer & 0x3U) != 1U) return -1;
    uint64_t address = (uint64_t)(pointer & 0xFFFFFF00U) << 8;
    uint32_t saved = rd32(probe, PBUS_PRAMIN_BASE);
    if (!address) address = ((uint64_t)saved << 16) + 0xF0000ULL;
    wr32(probe, PBUS_PRAMIN_BASE, (uint32_t)(address >> 16));
    uint32_t inside = (uint32_t)(address & 0xFFFFULL);
    for (uint32_t offset = 0; offset < sizeof(vbios_image); offset += 4U) {
        uint32_t value = rd32(probe, PRAMIN_WINDOW + inside + offset);
        vbios_image[offset + 0U] = (uint8_t)value;
        vbios_image[offset + 1U] = (uint8_t)(value >> 8);
        vbios_image[offset + 2U] = (uint8_t)(value >> 16);
        vbios_image[offset + 3U] = (uint8_t)(value >> 24);
    }
    wr32(probe, PBUS_PRAMIN_BASE, saved);
    probe->vbios = vbios_image;
    probe->vbios_bytes = sizeof(vbios_image);
    return vbios_image[0] == 0x55U && vbios_image[1] == 0xAAU ? 0 : -1;
}

static int find_gpu(uint8_t *bus_out, uint8_t *slot_out, uint8_t *function_out) {
    for (unsigned bus = 0; bus < 256U; bus++) {
        for (uint8_t slot = 0; slot < 32U; slot++) {
            uint32_t header = pci_config_read32((uint8_t)bus, slot, 0, 0x0CU);
            uint8_t functions = (header & 0x00800000U) ? 8U : 1U;
            for (uint8_t function = 0; function < functions; function++) {
                uint32_t id = pci_config_read32((uint8_t)bus, slot, function, 0);
                if ((id & 0xFFFFU) == 0xFFFFU) continue;
                uint32_t class_code = pci_config_read32((uint8_t)bus, slot, function, 0x08U);
                if ((id & 0xFFFFU) != PCI_VENDOR_NVIDIA || (class_code >> 24) != 0x03U) continue;
                *bus_out = (uint8_t)bus;
                *slot_out = slot;
                *function_out = function;
                return 0;
            }
        }
    }
    return -1;
}

int nv50_early_init(const struct boot_framebuffer_info *boot, struct boot_framebuffer_info *out) {
    if (!boot || !out || !boot_command_line_flag("nv50")) return -1;
    struct probe probe = {0};
    probe.log = log_buffer;
    probe.capacity = sizeof(log_buffer);
    log_buffer[0] = '\0';
    say(&probe, "nv50 early display setup\n");

    uint8_t bus = 0, slot = 0, function = 0;
    if (find_gpu(&bus, &slot, &function) != 0) {
        say(&probe, "  no nvidia display controller\n");
        log_bytes = probe.used;
        return -1;
    }
    uint32_t bar0 = pci_config_read32(bus, slot, function, 0x10U);
    uint32_t bar1_low = pci_config_read32(bus, slot, function, 0x14U);
    uint32_t bar1_high =
        ((bar1_low >> 1) & 3U) == 2U ? pci_config_read32(bus, slot, function, 0x18U) : 0;
    probe.bar1 = ((uint64_t)bar1_high << 32) | (bar1_low & 0xFFFFFFF0U);
    say_reg(&probe, "bar0           ", 0x10U, bar0);
    say_reg(&probe, "bar1 low       ", 0x14U, bar1_low);
    if ((bar0 & 1U) || !(bar0 & 0xFFFFFFF0U) || !probe.bar1) {
        say(&probe, "  bars are not assigned\n");
        log_bytes = probe.used;
        return -1;
    }
    probe.bar0 = vmm_map_device(bar0 & 0xFFFFFFF0U, NV_BAR0_BYTES);
    if (!probe.bar0) {
        say(&probe, "  bar0 not mapped\n");
        log_bytes = probe.used;
        return -1;
    }

    uint32_t boot0 = rd32(&probe, 0);
    uint32_t vram = rd32(&probe, 0x10020CU);
    probe.vram_bytes = vram & 0xFFFFFF00U;
    say_reg(&probe, "boot0          ", 0, boot0);
    say_reg(&probe, "vram size      ", 0x10020CU, vram);
    if (((boot0 >> 20) & 0x1F0U) != 0x50U && ((boot0 >> 20) & 0x1F0U) != 0x80U &&
        ((boot0 >> 20) & 0x1F0U) != 0x90U && ((boot0 >> 20) & 0x1F0U) != 0xA0U) {
        say(&probe, "  not an nv50 family chip\n");
        log_bytes = probe.used;
        return -1;
    }
    if (read_vbios(&probe) != 0) {
        say(&probe, "  no video bios image\n");
        log_bytes = probe.used;
        return -1;
    }

    probe.pramin_saved = rd32(&probe, PBUS_PRAMIN_BASE);
    state(&probe, "before");
    pramin_select(&probe, VRAM_INSTANCE);
    int ok = test_vram(&probe) == 0 && build_objects(&probe) == 0;
    if (ok) {
        take_over(&probe);
        ok = start_core(&probe) == 0 && query_capabilities(&probe) == 0 && modeset(&probe) == 0;
    }
    state(&probe, "after");
    wr32(&probe, PBUS_PRAMIN_BASE, probe.pramin_saved);
    say(&probe,
        ok ? "result: the console runs on our surface\n"
           : "result: stopped at the first failure\n");
    log_bytes = probe.used;
    if (!ok) return -1;

    *out = *boot;
    out->physical_address = probe.bar1 + VRAM_SURFACE;
    out->pitch = probe.surface_pitch;
    out->width = (uint16_t)probe.surface_width;
    out->height = (uint16_t)probe.surface_height;
    out->bits_per_pixel = 32;
    out->red_mask_size = 8;
    out->red_field_position = 16;
    out->green_mask_size = 8;
    out->green_field_position = 8;
    out->blue_mask_size = 8;
    out->blue_field_position = 0;
    out->reserved_mask_size = 8;
    out->reserved_field_position = 24;
    return 0;
}

const char *nv50_early_log(size_t *bytes) {
    if (bytes) *bytes = log_bytes;
    return log_bytes ? log_buffer : NULL;
}
