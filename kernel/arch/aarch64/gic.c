#include <stdint.h>

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

static uint64_t distributor;
static uint64_t redistributor;
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

static void distributor_defaults(void) {
    for (unsigned line = 32; line < lines; line += 32) {
        write32(distributor, GICD_ICENABLER + line / 8U, 0xFFFFFFFFU);
        write32(distributor, GICD_ICPENDR + line / 8U, 0xFFFFFFFFU);
        write32(distributor, GICD_IGROUPR + line / 8U, 0xFFFFFFFFU);
    }
    for (unsigned line = 32; line < lines; line++)
        *(volatile uint8_t *)(distributor + GICD_IPRIORITYR + line) = PRIORITY_DEFAULT;
}

static void init_v3(void) {
    uint64_t mpidr;
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
    uint64_t affinity = ((mpidr >> 32) & 0xFFULL) << 24 | (mpidr & 0xFFFFFFULL);

    write32(distributor, GICD_CTLR, 0);
    distributor_defaults();
    for (unsigned line = 32; line < lines; line++)
        write64(distributor, GICD_IROUTER + line * 8U, mpidr & 0xFF00FFFFFFULL);
    write32(distributor, GICD_CTLR, (1U << 4) | (1U << 1));

    uint64_t frames = aarch64_platform.gic_redistributor_size / GICR_FRAME_BYTES;
    if (frames > aarch64_platform.cpu_count && aarch64_platform.cpu_count) frames = aarch64_platform.cpu_count;
    if (!frames) frames = 1;
    uint64_t mapped = vmm_map_device(aarch64_platform.gic_redistributor, frames * GICR_FRAME_BYTES);
    if (!mapped) panic("GIC: redistributors could not be mapped");
    redistributor = 0;
    for (uint64_t frame = 0; frame < frames; frame++) {
        uint64_t candidate = mapped + frame * GICR_FRAME_BYTES;
        if ((read64(candidate, GICR_TYPER) >> 32) == affinity) {
            redistributor = candidate;
            break;
        }
    }
    if (!redistributor) redistributor = mapped;

    uint32_t waker = read32(redistributor, GICR_WAKER);
    write32(redistributor, GICR_WAKER, waker & ~(1U << 1));
    for (unsigned spin = 0; spin < 1000000U && (read32(redistributor, GICR_WAKER) & (1U << 2)); spin++) {
    }

    uint64_t sgi = redistributor + GICR_SGI_BASE;
    write32(sgi, GICD_ICENABLER, 0xFFFFFFFFU);
    write32(sgi, GICD_IGROUPR, 0xFFFFFFFFU);
    for (unsigned line = 0; line < 32; line++)
        *(volatile uint8_t *)(sgi + GICD_IPRIORITYR + line) = PRIORITY_DEFAULT;

    uint64_t sre;
    __asm__ volatile("mrs %0, ICC_SRE_EL1" : "=r"(sre));
    __asm__ volatile("msr ICC_SRE_EL1, %0; isb" : : "r"(sre | 1ULL) : "memory");
    __asm__ volatile("msr ICC_PMR_EL1, %0" : : "r"(0xF0ULL));
    __asm__ volatile("msr ICC_BPR1_EL1, %0" : : "r"(0ULL));
    __asm__ volatile("msr ICC_IGRPEN1_EL1, %0; isb" : : "r"(1ULL) : "memory");
}

static void init_v2(void) {
    write32(distributor, GICD_CTLR, 0);
    distributor_defaults();
    for (unsigned line = 32; line < lines; line++)
        *(volatile uint8_t *)(distributor + GICD_ITARGETSR + line) = 0x01U;
    for (unsigned line = 0; line < 32; line++)
        *(volatile uint8_t *)(distributor + GICD_IPRIORITYR + line) = PRIORITY_DEFAULT;
    write32(distributor, GICD_ICENABLER, 0xFFFFFFFFU);
    write32(distributor, GICD_CTLR, 1U);

    cpu_interface = vmm_map_device(aarch64_platform.gic_cpu_interface, 0x2000ULL);
    if (!cpu_interface) panic("GIC: cpu interface could not be mapped");
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
    if (aarch64_platform.gic_version == 3) init_v3();
    else init_v2();
    kprintf("GIC: v%d with %u lines\n", aarch64_platform.gic_version, lines);
}

void gic_enable_interrupt(uint32_t intid) {
    uint32_t bit = 1U << (intid % 32U);
    if (intid < 32U && aarch64_platform.gic_version == 3) {
        write32(redistributor + GICR_SGI_BASE, GICD_ISENABLER, bit);
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
