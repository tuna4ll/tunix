#include <stdint.h>

#include "../../include/percpu.h"
#include "../../include/vmm.h"
#include "aarch64.h"

extern void kprintf(const char *fmt, ...);
extern void panic(const char *message) __attribute__((noreturn));

#define GICD_CTLR 0x0000U
#define GICD_TYPER 0x0004U
#define GICD_IGROUPR 0x0080U
#define GICD_ISENABLER 0x0100U
#define GICD_ICENABLER 0x0180U
#define GICD_ICPENDR 0x0280U
#define GICD_IPRIORITYR 0x0400U
#define GICD_ITARGETSR 0x0800U
#define GICD_ICFGR 0x0C00U
#define GICD_SGIR 0x0F00U
#define GICD_IROUTER 0x6000U

#define GICR_TYPER 0x0008U
#define GICR_WAKER 0x0014U
#define GICR_SGI_BASE 0x10000U
#define GICR_FRAME_BYTES 0x20000ULL

#define GICC_CTLR 0x00U
#define GICC_PMR 0x04U
#define GICC_BPR 0x08U
#define GICC_IAR 0x0CU
#define GICC_EOIR 0x10U

#define PRIORITY_DEFAULT 0xA0U
#define MPIDR_AFFINITY_MASK 0xFF00FFFFFFULL

static uint64_t distributor;
static uint64_t redistributor_base;
static uint64_t redistributor_frames;
static uint64_t redistributors[SMP_MAX_CPUS];
static uint64_t redistributor_physical[SMP_MAX_CPUS];
static uint64_t cpu_interface;
static unsigned lines;

static uint32_t read32(uint64_t base, uint32_t offset) {
    return *(volatile uint32_t *)(base + offset);
}

static void write32(uint64_t base, uint32_t offset, uint32_t value) {
    *(volatile uint32_t *)(base + offset) = value;
}

static uint64_t read64(uint64_t base, uint32_t offset) {
    return *(volatile uint64_t *)(base + offset);
}

static void write64(uint64_t base, uint32_t offset, uint64_t value) {
    *(volatile uint64_t *)(base + offset) = value;
}

static uint64_t current_mpidr(void) {
    uint64_t value;
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(value));
    return value;
}

static void banked_defaults(uint64_t base) {
    write32(base, GICD_ICENABLER, 0xFFFFFFFFU);
    for (unsigned line = 0; line < 32; line++)
        *(volatile uint8_t *)(base + GICD_IPRIORITYR + line) = PRIORITY_DEFAULT;
}

static void distributor_defaults(void) {
    for (unsigned line = 32; line < lines; line += 32) {
        write32(distributor, GICD_ICENABLER + line / 8U, 0xFFFFFFFFU);
        write32(distributor, GICD_ICPENDR + line / 8U, 0xFFFFFFFFU);
        write32(distributor, GICD_IGROUPR + line / 8U, 0xFFFFFFFFU);
    }
    for (unsigned line = 32; line < lines; line++)
        *(volatile uint8_t *)(distributor + GICD_IPRIORITYR + line) = PRIORITY_DEFAULT;
}

static void cpu_local_v3(unsigned index) {
    uint64_t mpidr = current_mpidr();
    uint64_t affinity = ((mpidr >> 32) & 0xFFULL) << 24 | (mpidr & 0xFFFFFFULL);
    uint64_t found = 0;
    uint64_t found_frame = 0;
    for (uint64_t frame = 0; frame < redistributor_frames; frame++) {
        uint64_t candidate = redistributor_base + frame * GICR_FRAME_BYTES;
        if ((read64(candidate, GICR_TYPER) >> 32) == affinity) {
            found = candidate;
            found_frame = frame;
            break;
        }
    }
    if (!found) {
        if (index) panic("GIC: no redistributor for this processor");
        found = redistributor_base;
    }
    if (index < SMP_MAX_CPUS) {
        redistributors[index] = found;
        redistributor_physical[index] = aarch64_platform.gic_redistributor +
                                        found_frame * GICR_FRAME_BYTES;
    }

    uint32_t waker = read32(found, GICR_WAKER);
    write32(found, GICR_WAKER, waker & ~(1U << 1));
    for (unsigned spin = 0; spin < 1000000U && (read32(found, GICR_WAKER) & (1U << 2)); spin++) {
    }

    uint64_t sgi = found + GICR_SGI_BASE;
    banked_defaults(sgi);
    write32(sgi, GICD_IGROUPR, 0xFFFFFFFFU);

    uint64_t sre;
    __asm__ volatile("mrs %0, ICC_SRE_EL1" : "=r"(sre));
    __asm__ volatile("msr ICC_SRE_EL1, %0; isb" : : "r"(sre | 1ULL) : "memory");
    __asm__ volatile("msr ICC_PMR_EL1, %0" : : "r"(0xF0ULL));
    __asm__ volatile("msr ICC_BPR1_EL1, %0" : : "r"(0ULL));
    __asm__ volatile("msr ICC_IGRPEN1_EL1, %0; isb" : : "r"(1ULL) : "memory");
}

static void cpu_local_v2(void) {
    banked_defaults(distributor);
    write32(cpu_interface, GICC_PMR, 0xF0U);
    write32(cpu_interface, GICC_BPR, 0U);
    write32(cpu_interface, GICC_CTLR, 1U);
}

void gic_init(void) {
    if (!aarch64_platform.gic_version || !aarch64_platform.gic_distributor)
        panic("GIC: the device tree describes no interrupt controller");
    distributor = vmm_map_device(aarch64_platform.gic_distributor, 0x10000ULL);
    if (!distributor) panic("GIC: distributor could not be mapped");
    lines = ((read32(distributor, GICD_TYPER) & 0x1FU) + 1U) * 32U;
    if (lines > 1020U) lines = 1020U;

    write32(distributor, GICD_CTLR, 0);
    distributor_defaults();

    if (aarch64_platform.gic_version == 3) {
        uint64_t affinity = current_mpidr() & MPIDR_AFFINITY_MASK;
        for (unsigned line = 32; line < lines; line++)
            write64(distributor, GICD_IROUTER + line * 8U, affinity);
        write32(distributor, GICD_CTLR, (1U << 4) | (1U << 1));

        redistributor_frames = aarch64_platform.gic_redistributor_size / GICR_FRAME_BYTES;
        if (redistributor_frames > SMP_MAX_CPUS) redistributor_frames = SMP_MAX_CPUS;
        if (!redistributor_frames) redistributor_frames = 1;
        redistributor_base = vmm_map_device(aarch64_platform.gic_redistributor,
                                            redistributor_frames * GICR_FRAME_BYTES);
        if (!redistributor_base) panic("GIC: redistributors could not be mapped");
        cpu_local_v3(0);
        its_init();
    } else {
        for (unsigned line = 32; line < lines; line++)
            *(volatile uint8_t *)(distributor + GICD_ITARGETSR + line) = 0x01U;
        write32(distributor, GICD_CTLR, 1U);
        cpu_interface = vmm_map_device(aarch64_platform.gic_cpu_interface, 0x2000ULL);
        if (!cpu_interface) panic("GIC: cpu interface could not be mapped");
        cpu_local_v2();
    }
    gic_enable_interrupt(AARCH64_SGI_FLUSH);
    kprintf("GIC: v%d with %u lines\n", aarch64_platform.gic_version, lines);
}

void gic_init_secondary(unsigned index) {
    if (aarch64_platform.gic_version == 3) cpu_local_v3(index);
    else cpu_local_v2();
    gic_enable_interrupt(AARCH64_SGI_FLUSH);
}

void gic_enable_interrupt(uint32_t intid) {
    uint32_t bit = 1U << (intid % 32U);
    if (intid < 32U && aarch64_platform.gic_version == 3) {
        unsigned index = cpu_current()->index;
        uint64_t base = index < SMP_MAX_CPUS && redistributors[index] ? redistributors[index]
                                                                     : redistributor_base;
        write32(base + GICR_SGI_BASE, GICD_ISENABLER, bit);
        return;
    }
    write32(distributor, GICD_ISENABLER + (intid / 32U) * 4U, bit);
}

uint32_t gic_acknowledge(void) {
    if (aarch64_platform.gic_version == 3) {
        uint64_t value;
        __asm__ volatile("mrs %0, ICC_IAR1_EL1" : "=r"(value));
        return (uint32_t)value & 0xFFFFFFU;
    }
    return read32(cpu_interface, GICC_IAR) & 0x3FFU;
}

void gic_end_of_interrupt(uint32_t intid) {
    if (aarch64_platform.gic_version == 3) {
        __asm__ volatile("msr ICC_EOIR1_EL1, %0" : : "r"((uint64_t)intid));
        return;
    }
    write32(cpu_interface, GICC_EOIR, intid);
}

uint64_t gic_boot_redistributor(void) {
    return redistributors[0];
}

uint64_t gic_boot_redistributor_physical(void) {
    return redistributor_physical[0];
}

void gic_send_flush_ipi(void) {
    if (aarch64_platform.gic_version == 3) {
        uint64_t value = (1ULL << 40) | ((uint64_t)AARCH64_SGI_FLUSH << 24);
        __asm__ volatile("msr ICC_SGI1R_EL1, %0; isb" : : "r"(value) : "memory");
        return;
    }
    write32(distributor, GICD_SGIR, (1U << 24) | AARCH64_SGI_FLUSH);
}
