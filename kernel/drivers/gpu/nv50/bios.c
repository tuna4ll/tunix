#include <stddef.h>
#include <stdint.h>

#include <tunix/cpu.h>
#include <tunix/time.h>

#include "priv.h"

#define DCB_POINTER        0x36U
#define DCB_SIGNATURE      0x4EDCBDCBU
#define DCB_TYPE_LVDS      3U
#define DCB_TYPE_EOL       0xEU
#define OP_ZM_REG_SEQUENCE 0x58U
#define OP_SUB_DIRECT      0x5BU
#define OP_NV_REG          0x6EU
#define OP_DONE            0x71U
#define OP_TIME            0x74U
#define SCRIPT_MAX_STEPS   512U
#define SCRIPT_MAX_DEPTH   8U

extern void kprintf(const char *fmt, ...);

static uint8_t bios8(const struct nv50_device *gpu, uint32_t at) {
    return at < gpu->vbios_bytes ? gpu->vbios[at] : 0;
}

static uint16_t bios16(const struct nv50_device *gpu, uint32_t at) {
    return (uint16_t)(bios8(gpu, at) | (bios8(gpu, at + 1U) << 8));
}

static uint32_t bios32(const struct nv50_device *gpu, uint32_t at) {
    return (uint32_t)bios16(gpu, at) | ((uint32_t)bios16(gpu, at + 2U) << 16);
}

int nv50_bios_read(struct nv50_device *gpu, uint8_t *image, uint32_t capacity) {
    uint32_t pointer = nv50_rd32(gpu, PDISP_VBIOS_PTR);
    if (!(pointer & 0x8U) || (pointer & 0x3U) != 1U) return -1;
    uint64_t address = (uint64_t)(pointer & 0xFFFFFF00U) << 8;
    uint32_t saved = nv50_rd32(gpu, PBUS_PRAMIN_BASE);
    if (!address) address = ((uint64_t)saved << 16) + 0xF0000ULL;
    nv50_wr32(gpu, PBUS_PRAMIN_BASE, (uint32_t)(address >> 16));
    uint32_t inside = (uint32_t)(address & 0xFFFFULL);
    for (uint32_t offset = 0; offset + 4U <= capacity; offset += 4U) {
        uint32_t value = nv50_rd32(gpu, PRAMIN_WINDOW + inside + offset);
        image[offset + 0U] = (uint8_t)value;
        image[offset + 1U] = (uint8_t)(value >> 8);
        image[offset + 2U] = (uint8_t)(value >> 16);
        image[offset + 3U] = (uint8_t)(value >> 24);
    }
    nv50_wr32(gpu, PBUS_PRAMIN_BASE, saved);
    gpu->vbios = image;
    gpu->vbios_bytes = capacity;
    return image[0] == 0x55U && image[1] == 0xAAU ? 0 : -1;
}

static uint16_t bit_table(const struct nv50_device *gpu, uint8_t id, uint8_t *version) {
    for (uint32_t at = 0; at + 12U < gpu->vbios_bytes; at++) {
        if (bios8(gpu, at) != 0xFFU || bios8(gpu, at + 1U) != 0xB8U || bios8(gpu, at + 2U) != 'B' ||
            bios8(gpu, at + 3U) != 'I' || bios8(gpu, at + 4U) != 'T')
            continue;
        uint8_t size = bios8(gpu, at + 9U);
        uint8_t count = bios8(gpu, at + 10U);
        uint32_t entry = at + 12U;
        for (uint8_t index = 0; index < count; index++, entry += size) {
            if (bios8(gpu, entry) != id) continue;
            *version = bios8(gpu, entry + 1U);
            return bios16(gpu, entry + 4U);
        }
        return 0;
    }
    return 0;
}

static int find_dcb_lvds(struct nv50_device *gpu) {
    uint16_t dcb = bios16(gpu, DCB_POINTER);
    if (!dcb || bios8(gpu, dcb) < 0x40U || bios32(gpu, dcb + 6U) != DCB_SIGNATURE) return -1;
    uint8_t header = bios8(gpu, dcb + 1U);
    uint8_t count = bios8(gpu, dcb + 2U);
    uint8_t size = bios8(gpu, dcb + 3U);
    for (uint8_t index = 0; index < count; index++) {
        uint32_t entry = dcb + header + (uint32_t)index * size;
        uint32_t connection = bios32(gpu, entry);
        uint32_t config = bios32(gpu, entry + 4U);
        if ((connection & 0xFU) == DCB_TYPE_EOL) break;
        if ((connection & 0xFU) != DCB_TYPE_LVDS) continue;
        uint8_t location = (uint8_t)((connection >> 20) & 3U);
        uint8_t or_mask = (uint8_t)((connection >> 24) & 0x1FU);
        struct nv50_lvds *lvds = &gpu->lvds;
        lvds->link = (uint8_t)((config >> 4) & 3U);
        lvds->hash_type = (uint16_t)((location << 4) | DCB_TYPE_LVDS);
        lvds->or_index = (uint8_t)__builtin_ctz(or_mask ? or_mask : 1U);
        lvds->hash_mask =
            (uint16_t)(0x0100U | ((uint32_t)__builtin_ffs(lvds->link) << 6) | or_mask);
        return 0;
    }
    return -1;
}

static uint16_t clock_script(const struct nv50_device *gpu, uint16_t compare, uint32_t khz) {
    for (unsigned step = 0; compare && step < 32U; step++, compare = (uint16_t)(compare + 4U))
        if (khz / 10U >= bios16(gpu, compare)) return bios16(gpu, compare + 2U);
    return 0;
}

int nv50_bios_find_lvds(struct nv50_device *gpu, uint32_t khz) {
    if (find_dcb_lvds(gpu) != 0) {
        kprintf("NV50: the dcb has no lvds output\n");
        return -1;
    }
    uint8_t version = 0;
    uint16_t pointer = bit_table(gpu, 'U', &version);
    uint16_t table = pointer ? bios16(gpu, pointer) : 0;
    uint8_t table_version = bios8(gpu, table);
    if (!table || version != 1U || table_version < 0x20U || table_version > 0x22U) {
        kprintf("NV50: no display script table\n");
        return -1;
    }
    uint8_t header = bios8(gpu, table + 1U);
    uint8_t size = bios8(gpu, table + 2U);
    uint8_t count = bios8(gpu, table + 3U);
    uint8_t sub_header = bios8(gpu, table + 4U);
    struct nv50_lvds *lvds = &gpu->lvds;
    for (uint8_t index = 0; index < count; index++) {
        uint16_t output = bios16(gpu, table + header + (uint32_t)index * size);
        if (!output || sub_header < 0x0AU) continue;
        uint32_t mask = bios32(gpu, output + 2U);
        if (table_version <= 0x20U) mask |= 0xC0U;
        if (bios16(gpu, output) != lvds->hash_type || (mask & lvds->hash_mask) != lvds->hash_mask)
            continue;
        lvds->off_int1 = bios16(gpu, output + 8U);
        lvds->off_int2 = sub_header >= 0x0CU ? bios16(gpu, output + 0x0AU) : 0;
        uint8_t configs = bios8(gpu, output + 5U);
        for (uint8_t config = 0; config < configs; config++) {
            uint32_t at = output + sub_header + (uint32_t)config * 6U;
            uint8_t proto = bios8(gpu, at);
            if ((proto != 0U && proto != 0xFFU) || bios8(gpu, at + 1U) != 0U) continue;
            lvds->on_int2 = clock_script(gpu, bios16(gpu, at + 2U), khz);
            lvds->on_int3 = clock_script(gpu, bios16(gpu, at + 4U), khz);
            kprintf("NV50: lvds scripts on2 %x on3 %x off1 %x off2 %x\n", lvds->on_int2,
                    lvds->on_int3, lvds->off_int1, lvds->off_int2);
            return 0;
        }
    }
    kprintf("NV50: no display script matches the lvds output\n");
    return -1;
}

static uint32_t script_register(const struct nv50_lvds *lvds, uint32_t reg) {
    reg &= ~3U;
    reg &= ~0x80000000U;
    if (reg & 0x40000000U) {
        reg = (reg & ~0x40000000U) + lvds->or_index * 0x800U;
        if (reg & 0x20000000U) reg = (reg & ~0x20000000U) + (lvds->link == 2U) * 0x80U;
    }
    return reg;
}

static int run(struct nv50_device *gpu, uint16_t at, unsigned depth, unsigned *steps) {
    if (!at) return 0;
    if (depth > SCRIPT_MAX_DEPTH) return -1;
    for (;;) {
        if (++*steps > SCRIPT_MAX_STEPS) return -1;
        uint8_t op = bios8(gpu, at);
        if (op == OP_DONE) return 0;
        if (op == OP_SUB_DIRECT) {
            if (run(gpu, bios16(gpu, at + 1U), depth + 1U, steps) != 0) return -1;
            at = (uint16_t)(at + 3U);
        } else if (op == OP_ZM_REG_SEQUENCE) {
            uint32_t reg = bios32(gpu, at + 1U);
            uint8_t count = bios8(gpu, at + 5U);
            for (uint8_t index = 0; index < count; index++) {
                uint32_t target = script_register(&gpu->lvds, reg + index * 4U);
                if (target > 0xFFFFFCU) return -1;
                nv50_wr32(gpu, target, bios32(gpu, at + 6U + index * 4U));
            }
            at = (uint16_t)(at + 6U + count * 4U);
        } else if (op == OP_NV_REG) {
            uint32_t target = script_register(&gpu->lvds, bios32(gpu, at + 1U));
            if (target > 0xFFFFFCU) return -1;
            uint32_t keep = bios32(gpu, at + 5U);
            nv50_wr32(gpu, target, (nv50_rd32(gpu, target) & keep) | bios32(gpu, at + 9U));
            at = (uint16_t)(at + 13U);
        } else if (op == OP_TIME) {
            uint64_t until = time_uptime_ns() + (uint64_t)bios16(gpu, at + 1U) * 1000ULL;
            while (time_uptime_ns() < until) cpu_relax();
            at = (uint16_t)(at + 3U);
        } else {
            kprintf("NV50: script opcode %x at %x is not supported\n", op, at);
            return -1;
        }
    }
}

int nv50_bios_run(struct nv50_device *gpu, uint16_t script) {
    unsigned steps = 0;
    int result = run(gpu, script, 0, &steps);
    if (result != 0) kprintf("NV50: script %x stopped\n", script);
    return result;
}
