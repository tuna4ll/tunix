#include <stdint.h>

#include "../../include/cpu.h"

struct part_name {
    uint16_t part;
    const char *name;
};

static const struct part_name arm_parts[] = {
    { 0xD03, "Cortex-A53" }, { 0xD04, "Cortex-A35" }, { 0xD05, "Cortex-A55" },
    { 0xD07, "Cortex-A57" }, { 0xD08, "Cortex-A72" }, { 0xD09, "Cortex-A73" },
    { 0xD0A, "Cortex-A75" }, { 0xD0B, "Cortex-A76" }, { 0xD0C, "Neoverse-N1" },
    { 0xD0D, "Cortex-A77" }, { 0xD0E, "Cortex-A76AE" }, { 0xD40, "Neoverse-V1" },
    { 0xD41, "Cortex-A78" }, { 0xD44, "Cortex-X1" }, { 0xD46, "Cortex-A510" },
    { 0xD47, "Cortex-A710" }, { 0xD48, "Cortex-X2" }, { 0xD49, "Neoverse-N2" },
    { 0xD4B, "Cortex-A78C" }, { 0xD4F, "Neoverse-V2" }, { 0xD80, "Cortex-A520" },
    { 0xD81, "Cortex-A720" }, { 0xD82, "Cortex-X4" }, { 0xD87, "Cortex-A725" },
};

static const char *implementer_name(uint32_t implementer) {
    switch (implementer) {
    case 0x41: return "ARM";
    case 0x42: return "Broadcom";
    case 0x43: return "Cavium";
    case 0x48: return "HiSilicon";
    case 0x4E: return "NVIDIA";
    case 0x51: return "Qualcomm";
    case 0x61: return "Apple";
    case 0xC0: return "Ampere";
    default: return "unknown";
    }
}

static void copy_text(char *out, unsigned limit, const char *text) {
    unsigned length = 0;
    while (text[length] && length + 1 < limit) {
        out[length] = text[length];
        length++;
    }
    out[length] = '\0';
}

void cpu_identify(struct cpu_identity *out) {
    uint64_t midr;
    __asm__ volatile("mrs %0, midr_el1" : "=r"(midr));

    uint32_t implementer = (uint32_t)(midr >> 24) & 0xFFU;
    uint32_t part = (uint32_t)(midr >> 4) & 0xFFFU;

    copy_text(out->vendor, sizeof out->vendor, implementer_name(implementer));
    out->family = implementer;
    out->model_number = part;
    out->stepping = (uint32_t)midr & 0xFU;

    out->model[0] = '\0';
    if (implementer != 0x41) return;
    for (unsigned i = 0; i < sizeof arm_parts / sizeof arm_parts[0]; i++) {
        if (arm_parts[i].part == part) {
            copy_text(out->model, sizeof out->model, arm_parts[i].name);
            return;
        }
    }
}
