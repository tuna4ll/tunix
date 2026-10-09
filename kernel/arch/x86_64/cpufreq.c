#include <stddef.h>
#include <stdint.h>

#include <tunix/cpu.h>
#include <tunix/cpufreq.h>
#include <tunix/kstring.h>
#include <tunix/percpu.h>
#include <tunix/time.h>

extern void kprintf(const char *fmt, ...);

#define MSR_SMI_COUNT 0x34U
#define MSR_PLATFORM_INFO 0xCEU
#define MSR_MPERF 0xE7U
#define MSR_APERF 0xE8U
#define MSR_PERF_STATUS 0x198U
#define MSR_PERF_CTL 0x199U
#define MSR_MISC_ENABLE 0x1A0U

#define MISC_ENABLE_EIST (1ULL << 16)
#define MISC_ENABLE_TURBO_DISABLE (1ULL << 38)
#define SAMPLE_NS 1000000000ULL

struct core_state {
    struct cpufreq_reading reading;
    uint8_t initialised;
    uint64_t aperf;
    uint64_t mperf;
    uint64_t sampled_ns;
};

static int support = -1;
static unsigned ratio_shift;
static struct cpufreq_state state;
static struct core_state cores[SMP_MAX_CPUS];

static int atom_model(uint32_t model) {
    static const uint8_t atoms[] = { 0x1C, 0x26, 0x27, 0x35, 0x36, 0x37, 0x4A, 0x4C, 0x4D,
                                     0x5A, 0x5C, 0x5D, 0x5F, 0x7A, 0x86, 0x96, 0x9C, 0xAF,
                                     0xB6, 0xBE };
    for (unsigned index = 0; index < sizeof(atoms); index++)
        if (model == atoms[index]) return 1;
    return 0;
}

static int nehalem_model(uint32_t model) {
    return model == 0x1AU || model == 0x1EU || model == 0x1FU || model == 0x25U ||
           model == 0x2CU || model == 0x2EU || model == 0x2FU;
}

int cpufreq_supported(void) {
    if (support >= 0) return support;
    support = 0;
    struct cpu_identity identity;
    cpu_identify(&identity);
    if (strcmp(identity.vendor, "GenuineIntel") != 0 || identity.family != 6U) return 0;
    uint32_t model = identity.model_number;
    if (atom_model(model)) return 0;
    if (nehalem_model(model)) ratio_shift = 0;
    else if (model >= 0x2AU) ratio_shift = 8;
    else return 0;
    uint32_t a, b, c, d;
    cpu_cpuid(0, 0, &a, &b, &c, &d);
    if (a < 6U) return 0;
    cpu_cpuid(1, 0, &a, &b, &c, &d);
    if (!(c & (1U << 7))) return 0;
    cpu_cpuid(6, 0, &a, &b, &c, &d);
    state.measured = (c & 1U) != 0;
    int turbo_present = (a & 2U) != 0;

    uint64_t info = cpu_read_msr(MSR_PLATFORM_INFO);
    state.max_ratio = (uint32_t)((info >> 8) & 0xFFU);
    state.min_ratio = (uint32_t)((info >> 40) & 0xFFU);
    if (state.max_ratio < 4U || state.max_ratio > 80U) return 0;
    if (state.min_ratio < 1U || state.min_ratio > state.max_ratio) state.min_ratio = state.max_ratio;
    state.ratio_khz = time_tsc_frequency() / 1000ULL / state.max_ratio;
    state.smi_counted = 1;
    uint64_t misc = cpu_read_msr(MSR_MISC_ENABLE);
    state.eist_enabled = (misc & MISC_ENABLE_EIST) != 0;
    state.turbo = turbo_present && !(misc & MISC_ENABLE_TURBO_DISABLE);
    state.target_ratio = state.max_ratio + (state.turbo ? 1U : 0U);
    support = 1;
    return 1;
}

static uint32_t current_ratio(void) {
    return (uint32_t)((cpu_read_msr(MSR_PERF_STATUS) >> ratio_shift) & 0xFFU);
}

static void first_sample(struct core_state *core, unsigned index) {
    core->initialised = 1;
    core->reading.boot_ratio = current_ratio();
    if (!state.eist_enabled || core->reading.boot_ratio >= state.target_ratio) return;
    uint64_t control = cpu_read_msr(MSR_PERF_CTL);
    control &= ~(0xFFULL << ratio_shift);
    control |= (uint64_t)state.target_ratio << ratio_shift;
    cpu_write_msr(MSR_PERF_CTL, control);
    state.requested = 1;
    kprintf("CPUFREQ: cpu %u was at ratio %u, asked for %u\n", index,
            core->reading.boot_ratio, state.target_ratio);
}

void cpufreq_tick(void) {
    if (cpufreq_supported() <= 0) return;
    struct cpu *cpu = cpu_current();
    if (!cpu || cpu->index >= SMP_MAX_CPUS) return;
    struct core_state *core = &cores[cpu->index];
    uint64_t now = time_uptime_ns();
    if (core->sampled_ns && now - core->sampled_ns < SAMPLE_NS) return;
    if (!core->initialised) first_sample(core, cpu->index);
    core->sampled_ns = now ? now : 1;
    core->reading.ratio = current_ratio();
    core->reading.valid = 1;
    if (state.smi_counted && cpu->index == 0)
        state.smi_count = cpu_read_msr(MSR_SMI_COUNT) & 0xFFFFFFFFULL;
    if (!state.measured) return;
    uint64_t aperf = cpu_read_msr(MSR_APERF);
    uint64_t mperf = cpu_read_msr(MSR_MPERF);
    uint64_t busy = aperf - core->aperf;
    uint64_t reference = mperf - core->mperf;
    if (core->mperf && reference && busy < (1ULL << 40))
        core->reading.effective_khz = state.ratio_khz * state.max_ratio * busy / reference;
    core->aperf = aperf;
    core->mperf = mperf;
}

int cpufreq_read(unsigned index, struct cpufreq_reading *out) {
    if (cpufreq_supported() <= 0 || index >= SMP_MAX_CPUS || !out) return -1;
    *out = cores[index].reading;
    return out->valid ? 0 : -1;
}

const struct cpufreq_state *cpufreq_state(void) {
    return &state;
}
