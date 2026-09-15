#include <stdint.h>

#include "../../include/percpu.h"
#include "../../include/smp.h"
#include "aarch64.h"

extern void kprintf(const char *fmt, ...);

void smp_init(void) {
    percpu_mark_online(0);
    kprintf("SMP: %u processor(s) described, running on the first\n",
            aarch64_platform.cpu_count ? aarch64_platform.cpu_count : 1U);
}

unsigned smp_cpu_count(void) { return 1; }

void smp_flush_address_space(uint64_t cr3) { (void)cr3; }

void smp_service_flush(void) {
}

void smp_flush_interrupt(void) {
}
