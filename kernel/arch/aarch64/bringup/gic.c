#include <stdint.h>

#include "arch.h"

#define GICD_CTLR       (GICD_BASE + 0x0000)

#define GICR_SGI_BASE   (GICR_BASE + 0x10000)
#define GICR_WAKER      (GICR_BASE + 0x0014)
#define GICR_IGROUPR0   (GICR_SGI_BASE + 0x0080)
#define GICR_ISENABLER0 (GICR_SGI_BASE + 0x0100)
#define GICR_IPRIORITYR (GICR_SGI_BASE + 0x0400)

#define GICD_CTLR_ARE   (1U << 4)
#define GICD_CTLR_G1    (1U << 1)
#define GICD_CTLR_G0    (1U << 0)

#define WAKER_PROCESSOR_SLEEP (1U << 1)
#define WAKER_CHILDREN_ASLEEP (1U << 2)

static void gicr_wake(void) {
    uint32_t waker = mmio_read32(GICR_WAKER);
    waker &= ~WAKER_PROCESSOR_SLEEP;
    mmio_write32(GICR_WAKER, waker);
    while (mmio_read32(GICR_WAKER) & WAKER_CHILDREN_ASLEEP) {
    }
}

static void gicr_enable_ppi(uint32_t intid) {
    volatile uint8_t *prio = (volatile uint8_t *)GICR_IPRIORITYR;
    prio[intid] = 0x00;                                    // highest priority
    mmio_write32(GICR_IGROUPR0, mmio_read32(GICR_IGROUPR0) | (1U << intid));
    mmio_write32(GICR_ISENABLER0, (1U << intid));
}

void gic_init(void) {
    mmio_write32(GICD_CTLR, GICD_CTLR_ARE);
    dsb_sy();
    mmio_write32(GICD_CTLR, GICD_CTLR_ARE | GICD_CTLR_G1 | GICD_CTLR_G0);

    gicr_wake();
    gicr_enable_ppi(TIMER_PPI_INTID);

    sysreg_write("ICC_SRE_EL1", sysreg_read("ICC_SRE_EL1") | 1UL);
    isb();
    sysreg_write("ICC_PMR_EL1", 0xF0UL);
    sysreg_write("ICC_IGRPEN1_EL1", 1UL);
    isb();
}

uint32_t gic_acknowledge(void) {
    return (uint32_t)sysreg_read("ICC_IAR1_EL1");
}

void gic_eoi(uint32_t intid) {
    sysreg_write("ICC_EOIR1_EL1", intid);
}
