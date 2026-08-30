/*
 * The 16550 at 0x3F8, and the question of whether there is one.
 *
 * That question is not academic. A machine with nothing decoding the range
 * reads 0xFF from every register, and 0xFF says "a byte is waiting" as loudly
 * as a real byte does -- so the console's poll would read a byte, and another,
 * and another, inside the timer interrupt with the kernel lock held. What that
 * looks like from the front is a machine that stops a fraction of a second
 * after the first tick, having printed whatever it had printed, with no fault
 * and no message. The PS/2 driver has guarded against exactly this since it
 * was written; this one did not.
 *
 * So the port is probed once, and everything below is a no-op when it is not
 * there. The waits are bounded too: a port that is present but never reports
 * its transmit register empty must not be able to stop the kernel either.
 */
#include <stdint.h>

#include "../include/serial.h"

#define COM1 0x3F8U
#define COM1_INTERRUPT_ENABLE (COM1 + 1U)
#define COM1_FIFO (COM1 + 2U)
#define COM1_LINE_CONTROL (COM1 + 3U)
#define COM1_MODEM_CONTROL (COM1 + 4U)
#define COM1_LINE_STATUS (COM1 + 5U)
#define COM1_SCRATCH (COM1 + 7U)

#define LINE_STATUS_DATA_READY 0x01U
#define LINE_STATUS_TRANSMIT_EMPTY 0x20U

/* Long enough that a real port always finishes, short enough that a broken one
   costs a tick rather than the machine. */
#define TRANSMIT_SPINS 100000U
/* No real tick delivers more than a handful of bytes; a port answering with
   the same byte for ever delivers this many and is then left alone. */
#define READ_LIMIT 64U

static int present;

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ( "outb %0, %1" : : "a"(val), "Nd"(port) );
}

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ( "inb %1, %0" : "=a"(ret) : "Nd"(port) );
    return ret;
}

/*
 * The scratch register is the one register of a 16550 that does nothing but
 * remember what was written to it, which makes it the test for whether one is
 * there at all: an absent port reads back 0xFF, and a decoded-but-dead one
 * reads back zero.
 */
static int probe(void) {
    outb(COM1_SCRATCH, 0xAEU);
    if (inb(COM1_SCRATCH) != 0xAEU) return 0;
    outb(COM1_SCRATCH, 0x51U);
    if (inb(COM1_SCRATCH) != 0x51U) return 0;
    return 1;
}

void serial_init(void) {
    present = probe();
    if (!present) return;

    outb(COM1_INTERRUPT_ENABLE, 0x00U);
    outb(COM1_LINE_CONTROL, 0x80U);
    outb(COM1 + 0U, 0x03U);
    outb(COM1_INTERRUPT_ENABLE, 0x00U);
    outb(COM1_LINE_CONTROL, 0x03U);
    outb(COM1_FIFO, 0xC7U);
    outb(COM1_MODEM_CONTROL, 0x0BU);
}

int serial_present(void) { return present; }

void serial_write_char(char c) {
    if (!present) return;
    for (unsigned spin = 0; spin < TRANSMIT_SPINS; spin++) {
        if (inb(COM1_LINE_STATUS) & LINE_STATUS_TRANSMIT_EMPTY) {
            outb(COM1, (uint8_t)c);
            return;
        }
    }
}

/* One byte, or -1 when there is none. */
int serial_read_char(void) {
    if (!present) return -1;
    uint8_t status = inb(COM1_LINE_STATUS);
    /* All ones is an absent port, not a byte waiting -- and the probe above
       can be fooled by a port that appears after it, so this is checked here
       as well rather than trusted to have been settled once. */
    if (status == 0xFFU) return -1;
    if (!(status & LINE_STATUS_DATA_READY)) return -1;
    return inb(COM1);
}

unsigned serial_read_limit(void) { return READ_LIMIT; }
