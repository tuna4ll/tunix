#include <stddef.h>
#include <stdint.h>

#include <tunix/cpu.h>
#include <tunix/kstring.h>
#include <tunix/percpu.h>
#include <tunix/power.h>
#include <tunix/thermal.h>
#include <tunix/time.h>
#include <tunix/workqueue.h>

extern void kprintf(const char *fmt, ...);

#define MSR_CLOCK_MODULATION   0x19AU
#define MSR_THERM_STATUS       0x19CU
#define MSR_MISC_ENABLE        0x1A0U
#define MSR_TEMPERATURE_TARGET 0x1A2U

#define THERM_STATUS_CRITICAL   (1ULL << 4)
#define THERM_STATUS_VALID      (1ULL << 31)
#define MISC_ENABLE_TM1         (1ULL << 3)
#define CLOCK_MODULATION_ENABLE (1ULL << 4)
#define CLOCK_MODULATION_HALF   (4ULL << 1)

#define SAMPLE_NS        1000000000ULL
#define THROTTLE_MARGIN  5
#define RELEASE_MARGIN   15
#define CRITICAL_MARGIN  1
#define CRITICAL_SAMPLES 3U
#define DEFAULT_TJMAX    100

struct core_state {
    struct thermal_reading reading;
    uint8_t initialised;
    uint8_t critical_samples;
};

static int support = -1;
static struct thermal_state state = {DEFAULT_TJMAX, -1, 0, 0, 0};
static struct core_state cores[SMP_MAX_CPUS];

int thermal_supported(void) {
    if (support >= 0) return support;
    support = 0;
    struct cpu_identity identity;
    cpu_identify(&identity);
    if (strcmp(identity.vendor, "GenuineIntel") != 0) return 0;
    uint32_t a, b, c, d;
    cpu_cpuid(0, 0, &a, &b, &c, &d);
    if (a < 6U) return 0;
    cpu_cpuid(1, 0, &a, &b, &c, &d);
    if (!(d & (1U << 22))) return 0;
    state.clock_modulation = 1;
    cpu_cpuid(6, 0, &a, &b, &c, &d);
    if (!(a & 1U)) return 0;
    support = 1;
    uint32_t model = identity.model_number;
    int older = model == 0x1CU || model == 0x1DU || model == 0x26U || model == 0x27U ||
        model == 0x35U || model == 0x36U;
    if (identity.family == 6U && model >= 0x1AU && !older) {
        int target = (int)((cpu_read_msr(MSR_TEMPERATURE_TARGET) >> 16) & 0xFFU);
        if (target >= 60 && target <= 130) state.tjmax = target;
    }
    return 1;
}

static void shutdown_now(void *unused) {
    (void)unused;
    power_off();
}

static struct work shutdown_work = WORK_INITIALIZER(shutdown_now, NULL);

static void first_sample(struct core_state *core) {
    core->initialised = 1;
    int automatic = (cpu_read_msr(MSR_MISC_ENABLE) & MISC_ENABLE_TM1) != 0;
    if (state.automatic_control < 0 || !automatic) state.automatic_control = automatic;
}

void thermal_tick(void) {
    if (thermal_supported() <= 0) return;
    struct cpu *cpu = cpu_current();
    if (!cpu || cpu->index >= SMP_MAX_CPUS) return;
    unsigned index = cpu->index;
    struct core_state *core = &cores[index];
    uint64_t now = time_uptime_ns();
    if (core->reading.sampled_ns && now - core->reading.sampled_ns < SAMPLE_NS) return;
    if (!core->initialised) first_sample(core);

    uint64_t status = cpu_read_msr(MSR_THERM_STATUS);
    core->reading.sampled_ns = now ? now : 1;
    if (!(status & THERM_STATUS_VALID)) {
        core->reading.valid = 0;
        return;
    }
    int celsius = state.tjmax - (int)((status >> 16) & 0x7FU);
    core->reading.valid = 1;
    core->reading.celsius = celsius;
    if (celsius > core->reading.peak) core->reading.peak = celsius;

    if (state.clock_modulation && !core->reading.throttled &&
        celsius >= state.tjmax - THROTTLE_MARGIN) {
        cpu_write_msr(MSR_CLOCK_MODULATION, CLOCK_MODULATION_ENABLE | CLOCK_MODULATION_HALF);
        core->reading.throttled = 1;
        __atomic_add_fetch(&state.throttle_events, 1, __ATOMIC_RELAXED);
        kprintf("THERMAL: cpu %u at %d C (limit %d C); running it at half speed\n", index, celsius,
                state.tjmax);
    } else if (core->reading.throttled && celsius <= state.tjmax - RELEASE_MARGIN) {
        cpu_write_msr(MSR_CLOCK_MODULATION, 0);
        core->reading.throttled = 0;
        kprintf("THERMAL: cpu %u back to %d C; full speed\n", index, celsius);
    }

    int critical = (status & THERM_STATUS_CRITICAL) || celsius >= state.tjmax - CRITICAL_MARGIN;
    core->critical_samples = critical ? (uint8_t)(core->critical_samples + 1U) : 0;
    if (core->critical_samples >= CRITICAL_SAMPLES &&
        !__atomic_exchange_n(&state.shutting_down, 1, __ATOMIC_ACQ_REL)) {
        kprintf("THERMAL: cpu %u held %d C for %u s; powering off before the hardware does\n",
                index, celsius, CRITICAL_SAMPLES);
        work_queue(&shutdown_work);
    }
}

int thermal_read(unsigned index, struct thermal_reading *out) {
    if (thermal_supported() <= 0 || index >= SMP_MAX_CPUS || !out) return -1;
    *out = cores[index].reading;
    return out->valid ? 0 : -1;
}

const struct thermal_state *thermal_state(void) { return &state; }
