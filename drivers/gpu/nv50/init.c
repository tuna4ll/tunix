#include <stddef.h>
#include <stdint.h>

#include <tunix/cpu.h>
#include <tunix/time.h>

#include "priv.h"
#include <tunix/kprintf.h>

// The video BIOS's init scripts: what the display runs for an output as it
// changes mode, and what the card runs to start itself. The opcodes and what
// each does follow nouveau's nvkm/subdev/bios/init.c (MIT), with which a
// script here has to agree step for step.

#define SCRIPT_MAX_STEPS 4096U
#define SCRIPT_MAX_DEPTH 8U

struct init {
    struct nv50_device *gpu;
    const struct nv50_output *outp;
    unsigned head;
    uint16_t offset;
    uint16_t repeat;
    uint16_t repend;
    // nouveau's flags: 1 runs, 2 a condition failed, 4 forced on.
    uint8_t execute;
    unsigned depth;
    unsigned steps;
    int failed;
    // A check: every script reached is walked, and nothing is written.
    int dry;
};

static uint8_t rd08(const struct init *init, uint32_t at) { return nv50_bios8(init->gpu, at); }
static uint16_t rd16(const struct init *init, uint32_t at) { return nv50_bios16(init->gpu, at); }
static uint32_t rd32(const struct init *init, uint32_t at) { return nv50_bios32(init->gpu, at); }

static int running(const struct init *init) {
    return init->execute == 1 || (init->execute & 5) == 5;
}

static void set_running(struct init *init, int run) {
    if (run) init->execute &= 0xFDU;
    else init->execute |= 0x02U;
}

// Display scripts name registers relative to the head (bit 31), the output
// resource (bit 30) and, within that, the link (bit 29).
static uint32_t nvreg(const struct init *init, uint32_t reg) {
    reg &= ~3U;
    if (reg & 0x80000000U) reg = (reg & ~0x80000000U) + init->head * 0x800U;
    if (reg & 0x40000000U) {
        unsigned or_index = init->outp ? init->outp->or_index : 0;
        reg = (reg & ~0x40000000U) + or_index * 0x800U;
        if (reg & 0x20000000U) {
            unsigned link = init->outp && init->outp->link == 2U ? 1U : 0U;
            reg = (reg & ~0x20000000U) + link * 0x80U;
        }
    }
    return reg;
}

static uint32_t rd_reg(struct init *init, uint32_t reg) {
    reg = nvreg(init, reg);
    if (reg > 0xFFFFFCU) {
        init->failed = 1;
        return 0;
    }
    return running(init) ? nv50_rd32(init->gpu, reg) : 0;
}

static void wr_reg(struct init *init, uint32_t reg, uint32_t value) {
    reg = nvreg(init, reg);
    if (reg > 0xFFFFFCU) {
        init->failed = 1;
        return;
    }
    if (running(init)) nv50_wr32(init->gpu, reg, value);
}

static void mask_reg(struct init *init, uint32_t reg, uint32_t mask, uint32_t value) {
    uint32_t old = rd_reg(init, reg);
    wr_reg(init, reg, (old & ~mask) | value);
}

static void delay_us(uint64_t us) {
    uint64_t until = time_uptime_ns() + us * 1000ULL;
    while (time_uptime_ns() < until) cpu_relax();
}

// The BIT 'I' table: the script table and the condition table, at fixed
// offsets in it.
static uint16_t init_table(const struct init *init, unsigned at) {
    const struct nv50_device *gpu = init->gpu;
    if (!gpu->init_table || gpu->init_table_bytes < at + 2U) return 0;
    return rd16(init, gpu->init_table + at);
}

static int condition_met(struct init *init, uint8_t cond) {
    uint16_t table = init_table(init, 0x06U);
    if (!table) return 0;
    uint32_t reg = rd32(init, table + cond * 12U);
    uint32_t mask = rd32(init, table + cond * 12U + 4U);
    uint32_t value = rd32(init, table + cond * 12U + 8U);
    return (rd_reg(init, reg) & mask) == value;
}

static int execute(struct init *init);

static void call(struct init *init, uint16_t script, unsigned length) {
    if (script && (running(init) || init->dry)) {
        if (init->depth >= SCRIPT_MAX_DEPTH) {
            init->failed = 1;
            return;
        }
        uint16_t saved = init->offset;
        init->offset = script;
        init->depth++;
        if (execute(init) != 0) init->failed = 1;
        init->depth--;
        init->offset = saved;
    }
    init->offset = (uint16_t)(init->offset + length);
}

static int step(struct init *init) {
    uint16_t at = init->offset;
    uint8_t op = rd08(init, at);
    switch (op) {
    case 0x33: { // REPEAT
        uint8_t count = rd08(init, at + 1U);
        uint16_t outer = init->repeat;
        init->offset = (uint16_t)(at + 2U);
        init->repeat = init->offset;
        init->repend = init->offset;
        while (count--) {
            init->offset = init->repeat;
            if (execute(init) != 0) return -1;
        }
        init->offset = init->repend;
        init->repeat = outer;
        return 0;
    }
    case 0x36: // END_REPEAT
        init->offset = (uint16_t)(at + 1U);
        if (init->repeat) {
            init->repend = init->offset;
            init->offset = 0;
        }
        return 0;
    case 0x38: // NOT
        init->offset = (uint16_t)(at + 1U);
        init->execute ^= 0x02U;
        return 0;
    case 0x3A: { // GENERIC_CONDITION
        uint8_t cond = rd08(init, at + 1U);
        uint8_t size = rd08(init, at + 2U);
        init->offset = (uint16_t)(at + 3U);
        switch (cond) {
        case 0: // an internal DisplayPort panel
            if (!init->outp || init->outp->connector_type != NV50_CONNECTOR_EDP)
                set_running(init, 0);
            break;
        case 1: // which DisplayPort PLL: nothing but DP outputs ask
        case 2: break;
        case 5: // DP ASSR, which takes an aux channel this driver has not
        case 7: // no panel sequence delays
            set_running(init, 0);
            break;
        default:
            kprintf("NV50: generic condition %x at %x is unknown\n", cond, at);
            init->offset = (uint16_t)(init->offset + size);
            break;
        }
        return 0;
    }
    case 0x47: // ANDN_REG
        mask_reg(init, rd32(init, at + 1U), rd32(init, at + 5U), 0);
        init->offset = (uint16_t)(at + 9U);
        return 0;
    case 0x48: // OR_REG
        mask_reg(init, rd32(init, at + 1U), 0, rd32(init, at + 5U));
        init->offset = (uint16_t)(at + 9U);
        return 0;
    case 0x56: { // CONDITION_TIME: up to two seconds for it to hold
        uint8_t cond = rd08(init, at + 1U);
        unsigned wait = rd08(init, at + 2U) * 50U;
        if (wait > 100U) wait = 100U;
        init->offset = (uint16_t)(at + 3U);
        if (!running(init)) return 0;
        while (wait--) {
            if (condition_met(init, cond)) return 0;
            delay_us(20000);
        }
        set_running(init, 0);
        return 0;
    }
    case 0x57: // LTIME, milliseconds
        if (running(init)) delay_us((uint64_t)rd16(init, at + 1U) * 1000ULL);
        init->offset = (uint16_t)(at + 3U);
        return 0;
    case 0x58: { // ZM_REG_SEQUENCE
        uint32_t base = rd32(init, at + 1U);
        uint8_t count = rd08(init, at + 5U);
        for (uint8_t index = 0; index < count; index++)
            wr_reg(init, base + index * 4U, rd32(init, at + 6U + index * 4U));
        init->offset = (uint16_t)(at + 6U + count * 4U);
        return 0;
    }
    case 0x5B: // SUB_DIRECT
        call(init, rd16(init, at + 1U), 3);
        return 0;
    case 0x5C: // JUMP
        init->offset = running(init) ? rd16(init, at + 1U) : (uint16_t)(at + 3U);
        return 0;
    case 0x5F: { // COPY_NV_REG
        uint32_t source = rd32(init, at + 1U);
        uint8_t shift = rd08(init, at + 5U);
        uint32_t source_mask = rd32(init, at + 6U);
        uint32_t flip = rd32(init, at + 10U);
        uint32_t target = rd32(init, at + 14U);
        uint32_t keep = rd32(init, at + 18U);
        uint32_t data = rd_reg(init, source);
        data = (shift & 0x80U) ? data << (0x100U - shift) : data >> shift;
        mask_reg(init, target, ~keep, (data & source_mask) ^ flip);
        init->offset = (uint16_t)(at + 22U);
        return 0;
    }
    case 0x6B: { // SUB, by index in the script table
        uint16_t table = init_table(init, 0x00U);
        call(init, table ? rd16(init, table + rd08(init, at + 1U) * 2U) : 0, 2);
        return 0;
    }
    case 0x6E: // NV_REG
        mask_reg(init, rd32(init, at + 1U), ~rd32(init, at + 5U), rd32(init, at + 9U));
        init->offset = (uint16_t)(at + 13U);
        return 0;
    case 0x71: // DONE
        init->offset = 0;
        return 0;
    case 0x72: // RESUME
        init->offset = (uint16_t)(at + 1U);
        set_running(init, 1);
        return 0;
    case 0x74: { // TIME, microseconds
        uint16_t us = rd16(init, at + 1U);
        if (running(init)) delay_us(us < 1000U ? us : (us + 900U) / 1000U * 1000U);
        init->offset = (uint16_t)(at + 3U);
        return 0;
    }
    case 0x75: // CONDITION
        if (!condition_met(init, rd08(init, at + 1U))) set_running(init, 0);
        init->offset = (uint16_t)(at + 2U);
        return 0;
    case 0x77: // ZM_REG16
        wr_reg(init, rd32(init, at + 1U), rd16(init, at + 5U));
        init->offset = (uint16_t)(at + 7U);
        return 0;
    case 0x7A: { // ZM_REG; PMC_ENABLE keeps the bit that keeps the bus alive
        uint32_t reg = rd32(init, at + 1U);
        uint32_t data = rd32(init, at + 5U);
        if (reg == 0x000200U) data |= 1U;
        wr_reg(init, reg, data);
        init->offset = (uint16_t)(at + 9U);
        return 0;
    }
    case 0x90: // COPY_ZM_REG
        wr_reg(init, rd32(init, at + 5U), rd_reg(init, rd32(init, at + 1U)));
        init->offset = (uint16_t)(at + 9U);
        return 0;
    case 0x91: { // ZM_REG_GROUP
        uint32_t reg = rd32(init, at + 1U);
        uint8_t count = rd08(init, at + 5U);
        for (uint8_t index = 0; index < count; index++)
            wr_reg(init, reg, rd32(init, at + 6U + index * 4U));
        init->offset = (uint16_t)(at + 6U + count * 4U);
        return 0;
    }
    case 0x97: { // ZM_MASK_ADD
        uint32_t reg = rd32(init, at + 1U);
        uint32_t mask = rd32(init, at + 5U);
        uint32_t add = rd32(init, at + 9U);
        uint32_t data = rd_reg(init, reg);
        wr_reg(init, reg, (data & mask) | ((data + add) & ~mask));
        init->offset = (uint16_t)(at + 13U);
        return 0;
    }
    default:
        kprintf("NV50: script opcode %x at %x is not supported\n", op, at);
        return -1;
    }
}

static int execute(struct init *init) {
    while (init->offset) {
        if (++init->steps > SCRIPT_MAX_STEPS || init->failed) return -1;
        if (step(init) != 0) return -1;
    }
    return init->failed ? -1 : 0;
}

int nv50_init_run(struct nv50_device *gpu, uint16_t script, const struct nv50_output *outp,
                  unsigned head) {
    if (!script) return 0;
    struct init init = {.gpu = gpu, .outp = outp, .head = head, .offset = script, .execute = 1};
    int result = execute(&init);
    if (result != 0) kprintf("NV50: script %x stopped at %x\n", script, init.offset);
    return result;
}

// What a script would touch, without touching it: every opcode it reaches is
// one this interpreter knows, or the script is reported.
int nv50_init_check(struct nv50_device *gpu, uint16_t script, const struct nv50_output *outp) {
    if (!script) return 0;
    struct init init = {.gpu = gpu, .outp = outp, .offset = script, .execute = 0, .dry = 1};
    int result = execute(&init);
    if (result != 0) kprintf("NV50: script %x cannot be run, stopped at %x\n", script, init.offset);
    return result;
}
