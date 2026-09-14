#include <stdint.h>

#include "../../include/cpu.h"

static void copy_words(char *out, const uint32_t *words, unsigned count) {
    for (unsigned word = 0; word < count; word++)
        for (unsigned byte = 0; byte < 4; byte++)
            out[word * 4U + byte] = (char)((words[word] >> (byte * 8U)) & 0xFFU);
}

static void read_brand(char *model) {
    uint32_t a, b, c, d;
    model[0] = '\0';
    cpu_cpuid(0x80000000U, 0, &a, &b, &c, &d);
    if (a < 0x80000004U) return;

    char raw[49];
    for (unsigned leaf = 0; leaf < 3; leaf++) {
        cpu_cpuid(0x80000002U + leaf, 0, &a, &b, &c, &d);
        uint32_t words[4] = { a, b, c, d };
        copy_words(raw + leaf * 16U, words, 4);
    }
    raw[48] = '\0';

    unsigned begin = 0;
    while (raw[begin] == ' ') begin++;
    unsigned length = 0;
    while (raw[begin + length]) {
        model[length] = raw[begin + length];
        length++;
    }
    while (length && model[length - 1] == ' ') length--;
    model[length] = '\0';
}

void cpu_identify(struct cpu_identity *out) {
    uint32_t a, b, c, d;
    cpu_cpuid(0, 0, &a, &b, &c, &d);
    uint32_t words[3] = { b, d, c };
    copy_words(out->vendor, words, 3);
    out->vendor[12] = '\0';

    cpu_cpuid(1, 0, &a, &b, &c, &d);
    uint32_t family = (a >> 8) & 0xFU;
    uint32_t model = (a >> 4) & 0xFU;
    if (family == 0xFU) family += (a >> 20) & 0xFFU;
    if (family == 0x6U || family == 0xFU) model |= ((a >> 16) & 0xFU) << 4;
    out->family = family;
    out->model_number = model;
    out->stepping = a & 0xFU;

    read_brand(out->model);
}
