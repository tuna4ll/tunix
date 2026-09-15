#include <stdint.h>

#include "../include/serial.h"

#define LINE_STATUS_DATA_READY 0x01U
#define LINE_STATUS_TRANSMIT_EMPTY 0x20U

#define TRANSMIT_SPINS 100000U
#define READ_LIMIT 64U

static int present;

#if defined(__x86_64__)

#define COM1 0x3F8U
#define COM1_INTERRUPT_ENABLE (COM1 + 1U)
#define COM1_FIFO (COM1 + 2U)
#define COM1_LINE_CONTROL (COM1 + 3U)
#define COM1_MODEM_CONTROL (COM1 + 4U)
#define COM1_LINE_STATUS (COM1 + 5U)
#define COM1_SCRATCH (COM1 + 7U)

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ( "outb %0, %1" : : "a"(val), "Nd"(port) );
}

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ( "inb %1, %0" : "=a"(ret) : "Nd"(port) );
    return ret;
}

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

void serial_write_char(char c) {
    if (!present) return;
    for (unsigned spin = 0; spin < TRANSMIT_SPINS; spin++) {
        if (inb(COM1_LINE_STATUS) & LINE_STATUS_TRANSMIT_EMPTY) {
            outb(COM1, (uint8_t)c);
            return;
        }
    }
}

int serial_read_char(void) {
    if (!present) return -1;
    uint8_t status = inb(COM1_LINE_STATUS);
    if (status == 0xFFU) return -1;
    if (!(status & LINE_STATUS_DATA_READY)) return -1;
    return inb(COM1);
}

#else

#define UART_NONE 0
#define UART_NS16550 1
#define UART_PL011 2

#define NS16550_DATA 0U
#define NS16550_INTERRUPT_ENABLE 1U
#define NS16550_FIFO 2U
#define NS16550_LINE_CONTROL 3U
#define NS16550_MODEM_CONTROL 4U
#define NS16550_LINE_STATUS 5U

#define PL011_DR 0x00U
#define PL011_FR 0x18U
#define PL011_LCRH 0x2CU
#define PL011_CR 0x30U
#define PL011_IMSC 0x38U
#define PL011_FR_RXFE (1U << 4)
#define PL011_FR_TXFF (1U << 5)
#define PL011_CR_UARTEN (1U << 0)
#define PL011_CR_TXE (1U << 8)
#define PL011_CR_RXE (1U << 9)
#define PL011_LCRH_FEN (1U << 4)
#define PL011_LCRH_WORD8 (3U << 5)

static int kind;
static uint64_t base;
static unsigned register_shift;
static unsigned register_width;

void serial_attach_ns16550(uint64_t virtual_base, unsigned shift, unsigned width) {
    base = virtual_base;
    register_shift = shift;
    register_width = width;
    kind = UART_NS16550;
}

void serial_attach_pl011(uint64_t virtual_base) {
    base = virtual_base;
    kind = UART_PL011;
}

static uint32_t ns16550_read(unsigned reg) {
    uint64_t address = base + ((uint64_t)reg << register_shift);
    if (register_width == 4U) return *(volatile uint32_t *)address;
    return *(volatile uint8_t *)address;
}

static void ns16550_write(unsigned reg, uint32_t value) {
    uint64_t address = base + ((uint64_t)reg << register_shift);
    if (register_width == 4U) *(volatile uint32_t *)address = value;
    else *(volatile uint8_t *)address = (uint8_t)value;
}

static uint32_t pl011_read(unsigned reg) {
    return *(volatile uint32_t *)(base + reg);
}

static void pl011_write(unsigned reg, uint32_t value) {
    *(volatile uint32_t *)(base + reg) = value;
}

void serial_init(void) {
    present = 0;
    if (kind == UART_NS16550) {
        ns16550_write(NS16550_INTERRUPT_ENABLE, 0x00U);
        ns16550_write(NS16550_LINE_CONTROL, 0x03U);
        ns16550_write(NS16550_FIFO, 0x07U);
        ns16550_write(NS16550_MODEM_CONTROL, 0x03U);
        present = 1;
    } else if (kind == UART_PL011) {
        pl011_write(PL011_IMSC, 0);
        uint32_t control = pl011_read(PL011_CR);
        if (!(control & PL011_CR_UARTEN)) {
            pl011_write(PL011_LCRH, PL011_LCRH_WORD8 | PL011_LCRH_FEN);
            control = PL011_CR_UARTEN;
        }
        pl011_write(PL011_CR, control | PL011_CR_UARTEN | PL011_CR_TXE | PL011_CR_RXE);
        present = 1;
    }
}

void serial_write_char(char c) {
    if (!present) return;
    for (unsigned spin = 0; spin < TRANSMIT_SPINS; spin++) {
        if (kind == UART_PL011) {
            if (pl011_read(PL011_FR) & PL011_FR_TXFF) continue;
            pl011_write(PL011_DR, (uint8_t)c);
            return;
        }
        if (ns16550_read(NS16550_LINE_STATUS) & LINE_STATUS_TRANSMIT_EMPTY) {
            ns16550_write(NS16550_DATA, (uint8_t)c);
            return;
        }
    }
}

int serial_read_char(void) {
    if (!present) return -1;
    if (kind == UART_PL011) {
        if (pl011_read(PL011_FR) & PL011_FR_RXFE) return -1;
        return (int)(pl011_read(PL011_DR) & 0xFFU);
    }
    uint32_t status = ns16550_read(NS16550_LINE_STATUS);
    if ((status & 0xFFU) == 0xFFU) return -1;
    if (!(status & LINE_STATUS_DATA_READY)) return -1;
    return (int)(ns16550_read(NS16550_DATA) & 0xFFU);
}

#endif

int serial_present(void) { return present; }

unsigned serial_read_limit(void) { return READ_LIMIT; }
