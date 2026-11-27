#include <stddef.h>
#include <stdint.h>

#include "priv.h"
#include <tunix/kprintf.h>

uint8_t nv50_bios8(const struct nv50_device *gpu, uint32_t at) {
    return at < gpu->vbios_bytes ? gpu->vbios[at] : 0;
}

uint16_t nv50_bios16(const struct nv50_device *gpu, uint32_t at) {
    return (uint16_t)(nv50_bios8(gpu, at) | (nv50_bios8(gpu, at + 1U) << 8));
}

uint32_t nv50_bios32(const struct nv50_device *gpu, uint32_t at) {
    return (uint32_t)nv50_bios16(gpu, at) | ((uint32_t)nv50_bios16(gpu, at + 2U) << 16);
}

// The display engine points at the image the firmware ran, which may have
// been patched for this board; it is read through the PRAMIN window.
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

// An entry of the BIT table: its version, length and data.
uint16_t nv50_bit_entry(const struct nv50_device *gpu, uint8_t id, uint8_t *version,
                        uint16_t *length) {
    for (uint32_t at = 0; at + 12U < gpu->vbios_bytes; at++) {
        if (nv50_bios8(gpu, at) != 0xFFU || nv50_bios8(gpu, at + 1U) != 0xB8U ||
            nv50_bios8(gpu, at + 2U) != 'B' || nv50_bios8(gpu, at + 3U) != 'I' ||
            nv50_bios8(gpu, at + 4U) != 'T')
            continue;
        uint8_t header = nv50_bios8(gpu, at + 8U);
        uint8_t size = nv50_bios8(gpu, at + 9U);
        uint8_t count = nv50_bios8(gpu, at + 10U);
        uint32_t entry = at + header;
        for (uint8_t index = 0; index < count; index++, entry += size) {
            if (nv50_bios8(gpu, entry) != id) continue;
            if (version) *version = nv50_bios8(gpu, entry + 1U);
            if (length) *length = nv50_bios16(gpu, entry + 2U);
            return nv50_bios16(gpu, entry + 4U);
        }
        return 0;
    }
    return 0;
}

static uint16_t clock_script(const struct nv50_device *gpu, uint16_t compare, uint32_t khz) {
    for (unsigned step = 0; compare && step < 32U; step++, compare = (uint16_t)(compare + 4U))
        if (khz / 10U >= nv50_bios16(gpu, compare)) return nv50_bios16(gpu, compare + 2U);
    return 0;
}

// The panel's scripts in the display table (BIT 'U'), matched as nouveau
// matches an output: its type and location, and its link, OR and head.
int nv50_bios_find_lvds(struct nv50_device *gpu, uint32_t khz) {
    if (gpu->lvds_output < 0) {
        kprintf("NV50: the dcb has no lvds output\n");
        return -1;
    }
    const struct nv50_output *outp = &gpu->outputs[gpu->lvds_output];
    uint8_t version = 0;
    uint16_t pointer = nv50_bit_entry(gpu, 'U', &version, NULL);
    uint16_t table = pointer ? nv50_bios16(gpu, pointer) : 0;
    uint8_t table_version = nv50_bios8(gpu, table);
    if (!table || version != 1U || table_version < 0x20U || table_version > 0x22U) {
        kprintf("NV50: no display script table\n");
        return -1;
    }
    uint8_t header = nv50_bios8(gpu, table + 1U);
    uint8_t size = nv50_bios8(gpu, table + 2U);
    uint8_t count = nv50_bios8(gpu, table + 3U);
    uint8_t sub_header = nv50_bios8(gpu, table + 4U);
    struct nv50_lvds *lvds = &gpu->lvds;
    lvds->or_index = outp->or_index;
    lvds->link = outp->link;
    lvds->hash_type = outp->hash_type;
    lvds->hash_mask = (uint16_t)(0x0100U | (outp->link << 6) | (1U << outp->or_index));
    for (uint8_t index = 0; index < count; index++) {
        uint16_t output = nv50_bios16(gpu, table + header + (uint32_t)index * size);
        if (!output || sub_header < 0x0AU) continue;
        uint32_t mask = nv50_bios32(gpu, output + 2U);
        if (table_version <= 0x20U) mask |= 0xC0U;
        if (nv50_bios16(gpu, output) != lvds->hash_type ||
            (mask & lvds->hash_mask) != lvds->hash_mask)
            continue;
        lvds->off_int1 = nv50_bios16(gpu, output + 8U);
        lvds->off_int2 = sub_header >= 0x0CU ? nv50_bios16(gpu, output + 0x0AU) : 0;
        uint8_t configs = nv50_bios8(gpu, output + 5U);
        for (uint8_t config = 0; config < configs; config++) {
            uint32_t at = output + sub_header + (uint32_t)config * 6U;
            uint8_t proto = nv50_bios8(gpu, at);
            if ((proto != 0U && proto != 0xFFU) || nv50_bios8(gpu, at + 1U) != 0U) continue;
            lvds->on_int2 = clock_script(gpu, nv50_bios16(gpu, at + 2U), khz);
            lvds->on_int3 = clock_script(gpu, nv50_bios16(gpu, at + 4U), khz);
            kprintf("NV50: lvds scripts on2 %x on3 %x off1 %x off2 %x\n", lvds->on_int2,
                    lvds->on_int3, lvds->off_int1, lvds->off_int2);
            return 0;
        }
    }
    kprintf("NV50: no display script matches the lvds output\n");
    return -1;
}

int nv50_bios_run(struct nv50_device *gpu, uint16_t script) {
    const struct nv50_output *outp =
        gpu->lvds_output >= 0 ? &gpu->outputs[gpu->lvds_output] : NULL;
    return nv50_init_run(gpu, script, outp, 0);
}

// Walks, without running, every script the display table holds for each DCB
// output -- the ones it runs when the output is lit, turned off or clocked --
// and says whether this interpreter knows all they use.
void nv50_check_scripts(struct nv50_device *gpu) {
    uint8_t version = 0;
    uint16_t pointer = nv50_bit_entry(gpu, 'U', &version, NULL);
    uint16_t table = pointer ? nv50_bios16(gpu, pointer) : 0;
    uint8_t table_version = nv50_bios8(gpu, table);
    if (!table || version != 1U || table_version < 0x20U || table_version > 0x22U) return;
    uint8_t header = nv50_bios8(gpu, table + 1U);
    uint8_t size = nv50_bios8(gpu, table + 2U);
    uint8_t count = nv50_bios8(gpu, table + 3U);
    uint8_t sub_header = nv50_bios8(gpu, table + 4U);
    for (unsigned index = 0; index < gpu->output_count; index++) {
        const struct nv50_output *outp = &gpu->outputs[index];
        uint32_t want = ((uint32_t)outp->link << 6) | (1U << outp->or_index);
        unsigned scripts = 0, failures = 0, entries = 0;
        for (uint8_t entry = 0; entry < count; entry++) {
            uint16_t output = nv50_bios16(gpu, table + header + (uint32_t)entry * size);
            if (!output || sub_header < 0x0AU) continue;
            uint32_t mask = nv50_bios32(gpu, output + 2U);
            if (table_version <= 0x20U) mask |= 0xC0U;
            if ((nv50_bios16(gpu, output) & 0xFFU) != (outp->hash_type & 0xFFU) ||
                (mask & want) != want)
                continue;
            entries++;
            uint16_t list[3] = {nv50_bios16(gpu, output + 6U), nv50_bios16(gpu, output + 8U),
                                sub_header >= 0x0CU ? nv50_bios16(gpu, output + 0x0AU) : 0};
            for (unsigned at = 0; at < 3U; at++) {
                if (!list[at]) continue;
                scripts++;
                if (nv50_init_check(gpu, list[at], outp) != 0) failures++;
            }
            uint8_t configs = nv50_bios8(gpu, output + 5U);
            for (uint8_t config = 0; config < configs; config++) {
                uint32_t at = output + sub_header + (uint32_t)config * 6U;
                for (unsigned which = 0; which < 2U; which++) {
                    uint16_t compare = nv50_bios16(gpu, at + 2U + which * 2U);
                    for (unsigned step = 0; compare && step < 32U; step++, compare += 4U) {
                        uint16_t script = nv50_bios16(gpu, compare + 2U);
                        if (script) {
                            scripts++;
                            if (nv50_init_check(gpu, script, outp) != 0) failures++;
                        }
                        if (!nv50_bios16(gpu, compare)) break;
                    }
                }
            }
        }
        kprintf("NV50: output %u %s: %u display table entries, %u scripts, %u not runnable\n",
                index, nv50_output_name(outp), entries, scripts, failures);
    }
}
