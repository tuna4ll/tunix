#include <stdint.h>

#include "../../include/io.h"
#include "../../include/timer.h"

#define PIT_INPUT_HZ 1193182U
#define PIT_COMMAND 0x43U
#define PIT_CHANNEL0 0x40U
#define PIT_MODE_RATE_GENERATOR 0x34U

void arch_timer_start(unsigned hz) {
    uint32_t divisor = (PIT_INPUT_HZ + hz / 2U) / hz;
    if (divisor < 1U) divisor = 1U;
    if (divisor > 0xFFFFU) divisor = 0xFFFFU;

    outb(PIT_COMMAND, PIT_MODE_RATE_GENERATOR);
    outb(PIT_CHANNEL0, (uint8_t)(divisor & 0xFFU));
    outb(PIT_CHANNEL0, (uint8_t)(divisor >> 8));
}
