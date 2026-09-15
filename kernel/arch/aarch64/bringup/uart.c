#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include "arch.h"

#define UART_DR   (UART0_BASE + 0x00)
#define UART_FR   (UART0_BASE + 0x18)
#define UART_IBRD (UART0_BASE + 0x24)
#define UART_FBRD (UART0_BASE + 0x28)
#define UART_LCRH (UART0_BASE + 0x2C)
#define UART_CR   (UART0_BASE + 0x30)

#define FR_TXFF (1U << 5)

void uart_init(void) {
    mmio_write32(UART_CR, 0);
    mmio_write32(UART_IBRD, 26);
    mmio_write32(UART_FBRD, 3);
    mmio_write32(UART_LCRH, (3U << 5) | (1U << 4));
    mmio_write32(UART_CR, (1U << 0) | (1U << 8) | (1U << 9));
}

void uart_putc(char c) {
    if (c == '\n') uart_putc('\r');
    while (mmio_read32(UART_FR) & FR_TXFF) {
    }
    mmio_write32(UART_DR, (uint32_t)(unsigned char)c);
}

void uart_puts(const char *s) {
    while (*s) uart_putc(*s++);
}

static void print_unsigned(uint64_t value, unsigned base, int upper) {
    char buf[32];
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    int i = 0;
    do {
        buf[i++] = digits[value % base];
        value /= base;
    } while (value);
    while (i--) uart_putc(buf[i]);
}

void kprintf(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            uart_putc(*fmt);
            continue;
        }
        switch (*++fmt) {
        case 's': uart_puts(va_arg(args, const char *)); break;
        case 'c': uart_putc((char)va_arg(args, int)); break;
        case 'd': {
            int v = va_arg(args, int);
            if (v < 0) { uart_putc('-'); print_unsigned((uint64_t)(-(int64_t)v), 10, 0); }
            else print_unsigned((uint64_t)v, 10, 0);
            break;
        }
        case 'u': print_unsigned(va_arg(args, unsigned), 10, 0); break;
        case 'x': print_unsigned(va_arg(args, unsigned), 16, 0); break;
        case 'l': {
            unsigned long v = va_arg(args, unsigned long);
            if (*++fmt == 'x') print_unsigned(v, 16, 0);
            else if (*fmt == 'u') print_unsigned(v, 10, 0);
            else { print_unsigned(v, 10, 0); fmt--; }
            break;
        }
        case 'p': uart_puts("0x"); print_unsigned((uint64_t)va_arg(args, void *), 16, 0); break;
        case '%': uart_putc('%'); break;
        default: uart_putc('%'); uart_putc(*fmt); break;
        }
    }
    va_end(args);
}
