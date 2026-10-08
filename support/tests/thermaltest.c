#define TUNIX_CPU_H
#define TUNIX_PERCPU_H
#define TUNIX_POWER_H
#define TUNIX_TIME_H
#define TUNIX_WORKQUEUE_H
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

struct work {
    void (*run)(void *argument);
    void *argument;
    struct work *next;
    volatile uint32_t queued;
};

#define WORK_INITIALIZER(function, data) { (function), (data), 0, 0 }

static struct cpu_identity machine;
static uint32_t leaf1_edx;
static uint32_t leaf6_eax;
static uint32_t max_leaf = 0xB;
static uint64_t target_msr;
static uint64_t misc_msr;
static uint64_t therm_status[8];
static uint64_t clock_modulation[8];
static unsigned target_reads;
static unsigned status_reads;
static unsigned writes_other;
static unsigned misc_writes;
static struct cpu running;
static uint64_t clock_ns;
static unsigned queued_work;
static unsigned powered_off;
static char last_message[256];

static void cpu_identify(struct cpu_identity *out) {
    *out = machine;
}

static void cpu_cpuid(uint32_t leaf, uint32_t subleaf, uint32_t *a, uint32_t *b, uint32_t *c,
                      uint32_t *d) {
    (void)subleaf;
    *a = *b = *c = *d = 0;
    if (leaf == 0) *a = max_leaf;
    if (leaf == 1) *d = leaf1_edx;
    if (leaf == 6) *a = leaf6_eax;
}

static uint64_t cpu_read_msr(uint32_t msr) {
    if (msr == 0x1A2U) { target_reads++; return target_msr; }
    if (msr == 0x1A0U) return misc_msr;
    if (msr == 0x19CU) { status_reads++; return therm_status[running.index]; }
    if (msr == 0x19AU) return clock_modulation[running.index];
    fprintf(stderr, "unexpected rdmsr %x\n", msr);
    exit(2);
}

static void cpu_write_msr(uint32_t msr, uint64_t value) {
    if (msr == 0x19AU) { clock_modulation[running.index] = value; return; }
    if (msr == 0x1A0U) { misc_writes++; return; }
    writes_other++;
}

static struct cpu *cpu_current(void) {
    return &running;
}

static uint64_t time_uptime_ns(void) {
    return clock_ns;
}

static void work_queue(struct work *work) {
    queued_work++;
    work->run(work->argument);
}

static void power_off(void) {
    powered_off++;
}

void kprintf(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(last_message, sizeof(last_message), fmt, args);
    va_end(args);
}

#include "../../kernel/arch/x86_64/thermal.c"

static unsigned failures;

static void check(const char *name, int ok) {
    printf("THERMALTEST %s %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

static void reset(const char *vendor, uint32_t model, uint32_t edx, uint32_t dts) {
    support = -1;
    memset(&state, 0, sizeof(state));
    state.tjmax = DEFAULT_TJMAX;
    state.automatic_control = -1;
    memset(cores, 0, sizeof(cores));
    memset(&machine, 0, sizeof(machine));
    strcpy(machine.vendor, vendor);
    machine.family = 6;
    machine.model_number = model;
    leaf1_edx = edx;
    leaf6_eax = dts;
    target_msr = 105ULL << 16;
    misc_msr = 1ULL << 3;
    memset(therm_status, 0, sizeof(therm_status));
    memset(clock_modulation, 0, sizeof(clock_modulation));
    target_reads = status_reads = writes_other = misc_writes = 0;
    clock_ns = 1000;
    queued_work = powered_off = 0;
    running.index = 0;
}

static uint64_t reading(int below_limit) {
    return (1ULL << 31) | ((uint64_t)below_limit << 16);
}

static void tick_at(unsigned cpu, int below_limit, uint64_t seconds) {
    running.index = cpu;
    therm_status[cpu] = reading(below_limit);
    clock_ns = 1000 + seconds * 1000000000ULL;
    thermal_tick();
}

int main(void) {
    reset("AuthenticAMD", 0x25, 1U << 22, 1);
    thermal_tick();
    check("amd-unsupported", thermal_supported() == 0 && status_reads == 0);

    reset("GenuineIntel", 0x25, 1U << 22, 0);
    thermal_tick();
    check("no-dts-unsupported", thermal_supported() == 0 && status_reads == 0);

    reset("GenuineIntel", 0x25, 0, 1);
    thermal_tick();
    check("no-acpi-bit-unsupported", thermal_supported() == 0 && status_reads == 0);

    reset("GenuineIntel", 0x25, 1U << 22, 1);
    check("arrandale-reads-limit", thermal_supported() == 1 && state.tjmax == 105 &&
                                   target_reads == 1);

    reset("GenuineIntel", 0x1D, 1U << 22, 1);
    check("dunnington-skips-limit-msr", thermal_supported() == 1 && target_reads == 0 &&
                                        state.tjmax == 100);

    reset("GenuineIntel", 0x2A, 1U << 22, 1);
    target_msr = 0;
    check("zero-limit-keeps-default", thermal_supported() == 1 && state.tjmax == 100);

    reset("GenuineIntel", 0x25, 1U << 22, 1);
    tick_at(0, 40, 0);
    struct thermal_reading out;
    check("reads-celsius", thermal_read(0, &out) == 0 && out.celsius == 65 && out.peak == 65);
    check("tm1-reported-not-written", state.automatic_control == 1 && misc_writes == 0);
    unsigned before = status_reads;
    running.index = 0;
    clock_ns += 500000000ULL;
    thermal_tick();
    check("rate-limited", status_reads == before);

    tick_at(0, 6, 1);
    check("below-margin-not-throttled", clock_modulation[0] == 0 && !cores[0].reading.throttled);
    tick_at(0, 5, 2);
    check("throttles-at-margin", clock_modulation[0] == ((1ULL << 4) | (4ULL << 1)) &&
                                 cores[0].reading.throttled && state.throttle_events == 1);
    check("other-cpu-untouched", clock_modulation[1] == 0);
    tick_at(0, 10, 3);
    check("hysteresis-holds", cores[0].reading.throttled && clock_modulation[0] != 0);
    tick_at(0, 15, 4);
    check("releases-when-cool", !cores[0].reading.throttled && clock_modulation[0] == 0);
    check("peak-kept", thermal_read(0, &out) == 0 && out.peak == 100);

    reset("GenuineIntel", 0x25, 1U << 22, 1);
    running.index = 2;
    therm_status[2] = 0;
    clock_ns = 5;
    thermal_tick();
    check("invalid-reading-rejected", thermal_read(2, &out) != 0);

    reset("GenuineIntel", 0x25, 1U << 22, 1);
    tick_at(1, 1, 0);
    tick_at(1, 1, 1);
    check("two-critical-samples-wait", powered_off == 0);
    tick_at(1, 3, 2);
    check("cooling-resets-count", cores[1].critical_samples == 0 && powered_off == 0);
    tick_at(1, 0, 3);
    tick_at(1, 0, 4);
    tick_at(1, 1, 5);
    check("third-critical-sample-powers-off", powered_off == 1 && queued_work == 1 &&
                                             state.shutting_down);
    tick_at(3, 0, 6);
    tick_at(3, 0, 7);
    tick_at(3, 0, 8);
    check("powers-off-once", powered_off == 1 && queued_work == 1);

    reset("GenuineIntel", 0x25, 1U << 22, 1);
    for (unsigned second = 0; second < 3; second++) {
        running.index = 0;
        therm_status[0] = reading(20) | (1ULL << 4);
        clock_ns = 1000 + second * 1000000000ULL;
        thermal_tick();
    }
    check("critical-status-bit-counts", powered_off == 1);

    reset("GenuineIntel", 0x25, 1U << 22, 1);
    misc_msr = 0;
    tick_at(0, 40, 0);
    check("tm1-off-reported", state.automatic_control == 0 && misc_writes == 0);
    check("no-unexpected-writes", writes_other == 0);

    printf(failures ? "THERMALTEST FAIL\n" : "THERMALTEST PASS\n");
    return failures ? 1 : 0;
}
