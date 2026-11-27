#include <stddef.h>
#include <stdint.h>

#include "priv.h"
#include <tunix/kprintf.h>

// The DCB: the outputs a board has and what each is wired to, the i2c ports
// their monitors answer on, and the connectors. Laid out as nouveau's
// nvkm/subdev/bios/dcb.c, conn.c and i2c.c (MIT) read it, for DCB 3.0 and 4.x.

#define DCB_POINTER   0x36U
#define DCB_SIGNATURE 0x4EDCBDCBU
#define DCB_EOL       0xEU
#define DCB_UNUSED    0xFU

#define I2C_NVIO_BIT 5U
#define I2C_NVIO_AUX 6U

const char *nv50_output_name(const struct nv50_output *outp) {
    switch (outp->type) {
    case NV50_OUTPUT_CRT:  return "crt";
    case NV50_OUTPUT_TV:   return "tv";
    case NV50_OUTPUT_TMDS: return outp->connector_type == NV50_CONNECTOR_HDMI ? "hdmi" : "tmds";
    case NV50_OUTPUT_LVDS: return "lvds";
    case NV50_OUTPUT_DP:   return outp->connector_type == NV50_CONNECTOR_EDP ? "edp" : "dp";
    default:               return "other";
    }
}

static void parse_i2c(struct nv50_device *gpu, uint16_t table) {
    gpu->i2c_count = 0;
    if (!table) return;
    uint8_t version = nv50_bios8(gpu, table);
    if (version < 0x30U || version >= 0x41U) {
        kprintf("NV50: i2c table version %x is not read\n", version);
        return;
    }
    uint8_t header = nv50_bios8(gpu, table + 1U);
    uint8_t count = nv50_bios8(gpu, table + 2U);
    uint8_t size = nv50_bios8(gpu, table + 3U);
    for (uint8_t index = 0; index < count && index < NV50_I2C_PORTS; index++) {
        uint32_t entry = table + header + (uint32_t)index * size;
        struct nv50_i2c_port *port = &gpu->i2c[index];
        port->type = nv50_bios8(gpu, entry + 3U);
        port->drive = NV50_UNUSED;
        port->share = NV50_UNUSED;
        uint8_t first = nv50_bios8(gpu, entry), second = nv50_bios8(gpu, entry + 1U);
        if (port->type == I2C_NVIO_BIT) {
            port->drive = first & 0x0FU;
            if (second & 1U) port->share = second >> 1;
        } else if (port->type == I2C_NVIO_AUX) {
            if (second & 1U) port->share = first & 0x0FU;
        }
        gpu->i2c_count = index + 1U;
    }
}

static uint8_t connector_type(const struct nv50_device *gpu, uint16_t table, uint8_t index,
                              uint8_t *hpd) {
    *hpd = NV50_UNUSED;
    if (!table || nv50_bios8(gpu, table) < 0x30U) return NV50_UNUSED;
    uint8_t header = nv50_bios8(gpu, table + 1U);
    uint8_t count = nv50_bios8(gpu, table + 2U);
    uint8_t size = nv50_bios8(gpu, table + 3U);
    if (index >= count) return NV50_UNUSED;
    uint32_t entry = table + header + (uint32_t)index * size;
    uint8_t line = (uint8_t)((nv50_bios8(gpu, entry + 1U) & 0x30U) >> 4);
    if (size >= 4U) {
        line |= (uint8_t)((nv50_bios8(gpu, entry + 2U) & 0x03U) << 2);
        line |= (uint8_t)((nv50_bios8(gpu, entry + 3U) & 0x07U) << 4);
    }
    *hpd = line;
    return nv50_bios8(gpu, entry);
}

int nv50_dcb_parse(struct nv50_device *gpu) {
    uint8_t version = 0;
    uint16_t length = 0;
    gpu->init_table = nv50_bit_entry(gpu, 'I', &version, &length);
    gpu->init_table_bytes = length;
    gpu->output_count = 0;
    gpu->lvds_output = -1;

    uint16_t dcb = nv50_bios16(gpu, DCB_POINTER);
    uint8_t dcb_version = nv50_bios8(gpu, dcb);
    if (!dcb || dcb_version < 0x30U || nv50_bios32(gpu, dcb + 6U) != DCB_SIGNATURE) {
        kprintf("NV50: no dcb 3.0 or later\n");
        return -1;
    }
    uint8_t header = nv50_bios8(gpu, dcb + 1U);
    uint8_t count = nv50_bios8(gpu, dcb + 2U);
    uint8_t size = nv50_bios8(gpu, dcb + 3U);
    parse_i2c(gpu, nv50_bios16(gpu, dcb + 4U));
    uint16_t connectors = header >= 0x16U ? nv50_bios16(gpu, dcb + 0x14U) : 0;

    for (uint8_t index = 0; index < count && gpu->output_count < NV50_OUTPUTS; index++) {
        uint32_t entry = dcb + header + (uint32_t)index * size;
        uint32_t connection = nv50_bios32(gpu, entry);
        uint32_t config = nv50_bios32(gpu, entry + 4U);
        uint8_t type = connection & 0xFU;
        if (type == DCB_EOL) break;
        if (type == DCB_UNUSED) continue;
        struct nv50_output *outp = &gpu->outputs[gpu->output_count];
        outp->type = type;
        outp->i2c = (connection >> 4) & 0xFU;
        outp->heads = (connection >> 8) & 0xFU;
        outp->connector = (connection >> 12) & 0xFU;
        outp->location = (connection >> 20) & 3U;
        outp->or_mask = (connection >> 24) & 0xFU;
        outp->or_index = outp->or_mask ? (uint8_t)__builtin_ctz(outp->or_mask) : 0;
        outp->link = 0;
        if (dcb_version >= 0x40U &&
            (type == NV50_OUTPUT_TMDS || type == NV50_OUTPUT_LVDS || type == NV50_OUTPUT_DP))
            outp->link = (config >> 4) & 3U;
        outp->connector_type = connector_type(gpu, connectors, outp->connector, &outp->hpd);
        outp->hash_type = (uint16_t)((outp->location << 4) | type);
        outp->hash_mask = (uint16_t)((outp->heads << 8) | (outp->link << 6) | outp->or_mask);
        outp->edid_ok = 0;
        if (type == NV50_OUTPUT_LVDS && gpu->lvds_output < 0) gpu->lvds_output = (int)gpu->output_count;
        const struct nv50_i2c_port *port = outp->i2c < gpu->i2c_count ? &gpu->i2c[outp->i2c] : NULL;
        kprintf("NV50: output %u %s, %s %u, link %u, heads %x, connector %u type %x hpd %x, i2c %u",
                gpu->output_count, nv50_output_name(outp), type == NV50_OUTPUT_CRT ? "dac" : "sor",
                outp->or_index, outp->link, outp->heads, outp->connector, outp->connector_type,
                outp->hpd, outp->i2c);
        if (port)
            kprintf(" (type %x drive %x share %x)", port->type, port->drive, port->share);
        kprintf("\n");
        gpu->output_count++;
    }
    return gpu->output_count ? 0 : -1;
}
