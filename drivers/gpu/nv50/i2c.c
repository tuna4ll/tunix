#include <stddef.h>
#include <stdint.h>

#include <tunix/cpu.h>
#include <tunix/time.h>

#include "priv.h"
#include <tunix/kprintf.h>

// The DCB's i2c ports, bit-banged through the GPU's own pins as nouveau's
// nvkm/subdev/i2c/busnv50.c and bit.c (MIT) drive them, and what the monitor
// on each output says of itself through them (EDID).

#define T_RISEFALL_US 1U
#define T_HOLD_US     5U
#define T_TIMEOUT_US  2200U

#define I2C_NVIO_BIT 5U
#define EDID_ADDRESS 0x50U

static const uint32_t bus_registers[] = {
    0x00E138U, 0x00E150U, 0x00E168U, 0x00E180U, 0x00E254U,
    0x00E274U, 0x00E764U, 0x00E780U, 0x00E79CU, 0x00E7B8U,
};

struct bus {
    struct nv50_device *gpu;
    uint32_t reg;
    uint32_t data;
};

static void delay_us(unsigned us) {
    uint64_t until = time_uptime_ns() + (uint64_t)us * 1000ULL;
    while (time_uptime_ns() < until) nv50_relax();
}

static void drive_scl(struct bus *bus, int high) {
    bus->data = high ? bus->data | 1U : bus->data & ~1U;
    nv50_wr32(bus->gpu, bus->reg, bus->data);
}

static void drive_sda(struct bus *bus, int high) {
    bus->data = high ? bus->data | 2U : bus->data & ~2U;
    nv50_wr32(bus->gpu, bus->reg, bus->data);
}

static int sense_scl(struct bus *bus) { return (nv50_rd32(bus->gpu, bus->reg) & 1U) != 0; }
static int sense_sda(struct bus *bus) { return (nv50_rd32(bus->gpu, bus->reg) & 2U) != 0; }

// A device may hold the clock low while it gets ready.
static int raise_scl(struct bus *bus) {
    drive_scl(bus, 1);
    for (unsigned waited = 0; waited < T_TIMEOUT_US; waited += T_RISEFALL_US) {
        delay_us(T_RISEFALL_US);
        if (sense_scl(bus)) return 0;
    }
    return -1;
}

static int start(struct bus *bus) {
    int status = 0;
    if (!sense_scl(bus) || !sense_sda(bus)) {
        drive_scl(bus, 0);
        drive_sda(bus, 1);
        if (raise_scl(bus) != 0) status = -1;
    }
    drive_sda(bus, 0);
    delay_us(T_HOLD_US);
    drive_scl(bus, 0);
    delay_us(T_HOLD_US);
    return status;
}

static void stop(struct bus *bus) {
    drive_scl(bus, 0);
    drive_sda(bus, 0);
    delay_us(T_RISEFALL_US);
    drive_scl(bus, 1);
    delay_us(T_HOLD_US);
    drive_sda(bus, 1);
    delay_us(T_HOLD_US);
}

static int write_bit(struct bus *bus, int bit) {
    drive_sda(bus, bit);
    delay_us(T_RISEFALL_US);
    if (raise_scl(bus) != 0) return -1;
    delay_us(T_HOLD_US);
    drive_scl(bus, 0);
    delay_us(T_HOLD_US);
    return 0;
}

static int read_bit(struct bus *bus) {
    drive_sda(bus, 1);
    delay_us(T_RISEFALL_US);
    if (raise_scl(bus) != 0) return -1;
    delay_us(T_HOLD_US);
    int bit = sense_sda(bus);
    drive_scl(bus, 0);
    delay_us(T_HOLD_US);
    return bit;
}

// 0 when the byte was acknowledged.
static int put_byte(struct bus *bus, uint8_t byte) {
    for (int bit = 7; bit >= 0; bit--)
        if (write_bit(bus, (byte >> bit) & 1) != 0) return -1;
    return read_bit(bus) == 0 ? 0 : -1;
}

static int get_byte(struct bus *bus, uint8_t *byte, int last) {
    *byte = 0;
    for (int bit = 7; bit >= 0; bit--) {
        int value = read_bit(bus);
        if (value < 0) return -1;
        *byte = (uint8_t)(*byte | (value << bit));
    }
    return write_bit(bus, last ? 1 : 0);
}

// A pad shared with a DisplayPort aux channel is switched to i2c first.
static void pad_to_i2c(struct nv50_device *gpu, uint8_t share) {
    uint32_t base = (uint32_t)share * 0x50U;
    nv50_mask(gpu, 0x00E500U + base, 0x0000C003U, 0x0000C001U);
    nv50_mask(gpu, 0x00E50CU + base, 0x00000001U, 0);
}

// Writes OFFSET to the device at ADDRESS, then reads BYTES from it.
int nv50_i2c_read(struct nv50_device *gpu, unsigned port, uint8_t address, uint8_t offset,
                  uint8_t *data, unsigned bytes) {
    if (port >= gpu->i2c_count) return -1;
    const struct nv50_i2c_port *entry = &gpu->i2c[port];
    if (entry->type != I2C_NVIO_BIT || entry->drive >= sizeof bus_registers / sizeof bus_registers[0])
        return -1;
    if (entry->share != NV50_UNUSED) pad_to_i2c(gpu, entry->share);
    struct bus bus = {gpu, bus_registers[entry->drive], 7U};
    nv50_wr32(gpu, bus.reg, bus.data);
    int status = start(&bus);
    if (!status) status = put_byte(&bus, (uint8_t)(address << 1));
    if (!status) status = put_byte(&bus, offset);
    if (!status) status = start(&bus);
    if (!status) status = put_byte(&bus, (uint8_t)((address << 1) | 1U));
    for (unsigned index = 0; !status && index < bytes; index++)
        status = get_byte(&bus, &data[index], index + 1U == bytes);
    stop(&bus);
    return status;
}

// kprintf has no widths: a row of the EDID as two hex digits a byte.
static void print_row(const char *label, unsigned index, const uint8_t *row, unsigned bytes) {
    static const char digits[] = "0123456789abcdef";
    char text[2 * 32 + 1];
    for (unsigned at = 0; at < bytes && at < 32U; at++) {
        text[2 * at] = digits[row[at] >> 4];
        text[2 * at + 1] = digits[row[at] & 0xFU];
        text[2 * at + 2] = 0;
    }
    kprintf("NV50: output %u %s: %s\n", index, label, text);
}

static int edid_valid(const uint8_t *edid) {
    static const uint8_t header[8] = {0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00};
    uint8_t sum = 0;
    for (unsigned index = 0; index < 128U; index++) sum = (uint8_t)(sum + edid[index]);
    for (unsigned index = 0; index < 8U; index++)
        if (edid[index] != header[index]) return 0;
    return sum == 0;
}

static void report_edid(const struct nv50_output *outp, unsigned index) {
    const uint8_t *e = outp->edid;
    char vendor[4] = {(char)('@' + ((e[8] >> 2) & 0x1FU)),
                      (char)('@' + (((e[8] & 3U) << 3) | (e[9] >> 5))), (char)('@' + (e[9] & 0x1FU)),
                      0};
    // The monitor name descriptor (0xFC); panels often leave it out and put
    // their model in the unspecified text ones (0xFE) instead.
    char name[30] = {0};
    for (unsigned tag = 0xFCU, used = 0; tag <= 0xFEU && !used; tag += 2U) {
        for (unsigned d = 54; d < 126U; d += 18U) {
            if (e[d] || e[d + 1U] || e[d + 3U] != tag) continue;
            if (used && used + 1U < sizeof name) name[used++] = ' ';
            for (unsigned c = 0; c < 13U && e[d + 5U + c] != '\n' && used + 1U < sizeof name; c++)
                name[used++] = (char)e[d + 5U + c];
        }
    }
    unsigned clock = (unsigned)(e[54] | (e[55] << 8)) * 10U;
    unsigned h = e[56] | ((e[58] & 0xF0U) << 4), hblank = e[57] | ((e[58] & 0x0FU) << 8);
    unsigned v = e[59] | ((e[61] & 0xF0U) << 4), vblank = e[60] | ((e[61] & 0x0FU) << 8);
    unsigned hz = (h + hblank) && (v + vblank) ? clock * 1000U / ((h + hblank) * (v + vblank)) : 0;
    kprintf("NV50: output %u edid: %s product %x '%s', edid %u.%u, %ux%u cm, prefers %ux%u@%u"
            " (%u khz), %u extension(s)\n",
            index, vendor, (unsigned)(e[10] | (e[11] << 8)), name, e[18], e[19], e[21], e[22], h, v,
            hz, clock, e[126]);
    for (unsigned row = 0; row < 128U; row += 32U) print_row("edid", index, e + row, 32);
}

// Reads every output's EDID; a port nothing answers on is a connector with
// nothing plugged in, or one that is not wired to i2c.
void nv50_probe_outputs(struct nv50_device *gpu) {
    for (unsigned index = 0; index < gpu->output_count; index++) {
        struct nv50_output *outp = &gpu->outputs[index];
        uint64_t started = time_uptime_ns();
        int status = -1;
        for (int attempt = 0; attempt < 3 && status; attempt++) {
            status = nv50_i2c_read(gpu, outp->i2c, EDID_ADDRESS, 0, outp->edid, 128);
            if (!status && !edid_valid(outp->edid)) status = 1;
        }
        unsigned us = (unsigned)((time_uptime_ns() - started) / 1000ULL);
        outp->edid_ok = status == 0;
        if (outp->edid_ok) {
            report_edid(outp, index);
        } else {
            kprintf("NV50: output %u %s: %s on i2c %u (%u us)\n", index, nv50_output_name(outp),
                    status > 0 ? "an edid that does not check" : "no edid", outp->i2c, us);
            if (status > 0)
                for (unsigned row = 0; row < 128U; row += 32U)
                    print_row("bad edid", index, outp->edid + row, 32);
        }
    }
}
