#define TUNIX_CPU_H
#define TUNIX_PERCPU_H
#define TUNIX_TIME_H
#define TUNIX_KSTRING_H

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SMP_MAX_CPUS 256

struct cpu_identity {
    char vendor[13];
    char model[49];
    uint32_t family;
    uint32_t model_number;
    uint32_t stepping;
};

struct cpu {
    uint32_t index;
};

static struct cpu_identity machine;
static uint32_t leaf1_ecx;
static uint32_t leaf6_ecx;
static uint32_t leaf6_eax;
static uint64_t platform_info;
static uint64_t misc_msr;
static uint64_t perf_status[8];
static uint64_t perf_ctl[8];
static uint64_t aperf[8];
static uint64_t mperf[8];
static unsigned msr_reads;
static unsigned ctl_writes;
static struct cpu running;
static uint64_t clock_ns;
static char last_message[256];

static void cpu_identify(struct cpu_identity *out) {
    *out = machine;
}

static void cpu_cpuid(uint32_t leaf, uint32_t subleaf, uint32_t *a, uint32_t *b, uint32_t *c,
                      uint32_t *d) {
    (void)subleaf;
    *a = *b = *c = *d = 0;
    if (leaf == 0) *a = 0xB;
    if (leaf == 1) *c = leaf1_ecx;
    if (leaf == 6) { *a = leaf6_eax; *c = leaf6_ecx; }
}

static uint64_t cpu_read_msr(uint32_t msr) {
    msr_reads++;
    switch (msr) {
    case 0xCE: return platform_info;
    case 0x1A0: return misc_msr;
    case 0x198: return perf_status[running.index];
    case 0x199: return perf_ctl[running.index];
    case 0xE7: return mperf[running.index];
    case 0xE8: return aperf[running.index];
    default:
        fprintf(stderr, "unexpected rdmsr %x\n", msr);
        exit(2);
    }
}

static void cpu_write_msr(uint32_t msr, uint64_t value) {
    if (msr != 0x199) {
        fprintf(stderr, "unexpected wrmsr %x\n", msr);
        exit(2);
    }
    ctl_writes++;
    perf_ctl[running.index] = value;
}

static struct cpu *cpu_current(void) {
    return &running;
}

static uint64_t time_uptime_ns(void) {
    return clock_ns;
}

static uint64_t time_tsc_frequency(void) {
    return 2261274237ULL;
}

void kprintf(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(last_message, sizeof(last_message), fmt, args);
    va_end(args);
}

#include "../../kernel/arch/x86_64/cpufreq.c"

static unsigned failures;

static void check(const char *name, int ok) {
    printf("CPUFREQTEST %s %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

static void reset(const char *vendor, uint32_t model) {
    support = -1;
    ratio_shift = 0;
    memset(&state, 0, sizeof(state));
    memset(cores, 0, sizeof(cores));
    memset(&machine, 0, sizeof(machine));
    strcpy(machine.vendor, vendor);
    machine.family = 6;
    machine.model_number = model;
    leaf1_ecx = 1U << 7;
    leaf6_ecx = 1;
    leaf6_eax = 0;
    platform_info = (17ULL << 8) | (9ULL << 40);
    misc_msr = 1ULL << 16;
    memset(perf_status, 0, sizeof(perf_status));
    memset(perf_ctl, 0, sizeof(perf_ctl));
    memset(aperf, 0, sizeof(aperf));
    memset(mperf, 0, sizeof(mperf));
    msr_reads = ctl_writes = 0;
    running.index = 0;
    clock_ns = 1000;
    last_message[0] = 0;
}

int main(void) {
    reset("AuthenticAMD", 0x25);
    cpufreq_tick();
    check("amd-unsupported", cpufreq_supported() == 0 && msr_reads == 0);

    reset("GenuineIntel", 0x25);
    leaf1_ecx = 0;
    cpufreq_tick();
    check("no-speedstep-unsupported", cpufreq_supported() == 0 && msr_reads == 0);

    reset("GenuineIntel", 0x37);
    cpufreq_tick();
    check("atom-unsupported", cpufreq_supported() == 0 && msr_reads == 0);

    reset("GenuineIntel", 0x17);
    cpufreq_tick();
    check("penryn-unsupported", cpufreq_supported() == 0 && msr_reads == 0);

    reset("GenuineIntel", 0x25);
    platform_info = 0;
    check("bad-platform-info-unsupported", cpufreq_supported() == 0 && ctl_writes == 0);

    reset("GenuineIntel", 0x25);
    perf_status[0] = 9;
    perf_ctl[0] = (1ULL << 32) | 9;
    cpufreq_tick();
    struct cpufreq_reading reading;
    check("arrandale-low-byte-request", perf_ctl[0] == ((1ULL << 32) | 17) && ctl_writes == 1 &&
                                        state.target_ratio == 17 && !state.turbo);
    check("arrandale-boot-ratio-kept", cpufreq_read(0, &reading) == 0 && reading.boot_ratio == 9);
    check("arrandale-logs-the-change", strstr(last_message, "ratio 9, asked for 17") != NULL);
    check("range-from-tsc", state.ratio_khz * state.max_ratio / 1000U == 2261 &&
                            state.ratio_khz * state.min_ratio / 1000U == 1197);

    reset("GenuineIntel", 0x25);
    leaf6_eax = 2;
    perf_status[0] = 9;
    perf_ctl[0] = 9;
    cpufreq_tick();
    check("turbo-target-one-above", state.turbo && state.target_ratio == 18 && perf_ctl[0] == 18);

    reset("GenuineIntel", 0x25);
    leaf6_eax = 2;
    perf_status[0] = 18;
    perf_ctl[0] = 18;
    cpufreq_tick();
    check("firmware-turbo-left-alone", ctl_writes == 0 && !state.requested && perf_ctl[0] == 18 &&
                                       cpufreq_read(0, &reading) == 0 && reading.boot_ratio == 18);

    reset("GenuineIntel", 0x25);
    leaf6_eax = 2;
    misc_msr |= 1ULL << 38;
    perf_status[0] = 18;
    perf_ctl[0] = 18;
    cpufreq_tick();
    check("never-lowers-a-higher-ratio", ctl_writes == 0 && !state.turbo && perf_ctl[0] == 18);

    reset("GenuineIntel", 0x2A);
    platform_info = (34ULL << 8) | (16ULL << 40);
    perf_status[0] = 16ULL << 8;
    perf_ctl[0] = 0x1600;
    cpufreq_tick();
    check("sandybridge-high-byte-request", perf_ctl[0] == 0x2200 && ctl_writes == 1 &&
                                           cpufreq_read(0, &reading) == 0 &&
                                           reading.boot_ratio == 16);

    reset("GenuineIntel", 0x25);
    misc_msr = 0;
    perf_status[0] = 9;
    cpufreq_tick();
    check("firmware-disabled-no-write", ctl_writes == 0 && !state.requested && !state.eist_enabled);

    reset("GenuineIntel", 0x25);
    perf_status[0] = 17;
    aperf[0] = 1000;
    mperf[0] = 1000;
    cpufreq_tick();
    unsigned reads = msr_reads;
    clock_ns += 400000000ULL;
    cpufreq_tick();
    check("rate-limited", msr_reads == reads);
    aperf[0] += 1000000000ULL;
    mperf[0] += 2000000000ULL;
    clock_ns += 700000000ULL;
    cpufreq_tick();
    check("measures-half-speed", cpufreq_read(0, &reading) == 0 &&
                                 reading.effective_khz / 1000U == 1130);
    aperf[0] += 2000000000ULL;
    mperf[0] += 2000000000ULL;
    clock_ns += 1000000000ULL;
    cpufreq_tick();
    check("measures-full-speed", cpufreq_read(0, &reading) == 0 &&
                                 reading.effective_khz / 1000U == 2261 && reading.ratio == 17);

    reset("GenuineIntel", 0x25);
    leaf6_ecx = 0;
    perf_status[0] = 17;
    cpufreq_tick();
    check("no-aperf-no-measurement", cpufreq_read(0, &reading) == 0 && reading.effective_khz == 0);

    printf(failures ? "CPUFREQTEST FAIL\n" : "CPUFREQTEST PASS\n");
    return failures ? 1 : 0;
}
