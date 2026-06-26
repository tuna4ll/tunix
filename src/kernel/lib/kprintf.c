#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include "../include/klog.h"
#include "../include/kstring.h"

#define KLOG_CAPACITY 16384U

void serial_write_char(char c);

static char klog_buffer[KLOG_CAPACITY];
static size_t klog_head;
static size_t klog_count;

static void klog_store_char(char c) {
    size_t index = (klog_head + klog_count) % KLOG_CAPACITY;
    if (klog_count == KLOG_CAPACITY) {
        klog_head = (klog_head + 1U) % KLOG_CAPACITY;
        index = (klog_head + klog_count - 1U) % KLOG_CAPACITY;
    } else {
        klog_count++;
    }
    klog_buffer[index] = c;
}

static void emit_char(char c) {
    klog_store_char(c);
    serial_write_char(c);
}

size_t klog_size(void) {
    return klog_count;
}

int64_t klog_read(uint64_t offset, size_t size, void *buffer) {
    if (!buffer) return -1;
    if (offset >= klog_count) return 0;
    size_t available = klog_count - (size_t)offset;
    if (size > available) size = available;
    char *out = (char *)buffer;
    for (size_t i = 0; i < size; i++)
        out[i] = klog_buffer[(klog_head + (size_t)offset + i) % KLOG_CAPACITY];
    return (int64_t)size;
}

int64_t klog_write(size_t size, const void *buffer) {
    if (!buffer) return -1;
    const char *bytes = (const char *)buffer;
    for (size_t i = 0; i < size; i++) emit_char(bytes[i]);
    return (int64_t)size;
}

static void print_uint(uint64_t num, int base, int is_upper) {
    char buf[32];
    int i = 0;
    if (num == 0) {
        emit_char('0');
        return;
    }
    while (num > 0) {
        int rem = (int)(num % (uint64_t)base);
        if (rem < 10) buf[i++] = (char)(rem + '0');
        else buf[i++] = (char)(rem - 10 + (is_upper ? 'A' : 'a'));
        num /= (uint64_t)base;
    }
    while (i > 0) emit_char(buf[--i]);
}

static void print_int(int64_t num, int base, int is_upper) {
    if (num < 0) {
        emit_char('-');
        print_uint((uint64_t)(-(num + 1)) + 1U, base, is_upper);
    } else {
        print_uint((uint64_t)num, base, is_upper);
    }
}

void kprintf(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    while (*fmt) {
        if (*fmt == '%') {
            fmt++;
            if (*fmt == 's') {
                const char *s = va_arg(args, const char *);
                if (!s) s = "(null)";
                while (*s) emit_char(*s++);
            } else if (*fmt == 'c') {
                emit_char((char)va_arg(args, int));
            } else if (*fmt == 'd') {
                print_int(va_arg(args, int), 10, 0);
            } else if (*fmt == 'u') {
                print_uint(va_arg(args, unsigned int), 10, 0);
            } else if (*fmt == 'x') {
                print_uint(va_arg(args, unsigned int), 16, 0);
            } else if (*fmt == 'p') {
                emit_char('0');
                emit_char('x');
                print_uint((uint64_t)va_arg(args, void *), 16, 0);
            } else if (*fmt == '%') {
                emit_char('%');
            } else if (*fmt) {
                emit_char(*fmt);
            }
        } else {
            emit_char(*fmt);
        }
        if (*fmt) fmt++;
    }
    va_end(args);
}

extern void terminal_print(const char *);

void panic(const char *msg) {
    kprintf("PANIC: %s\n", msg);
    terminal_print("\n\n*** KERNEL PANIC ***\n");
    terminal_print(msg);
    while (1) __asm__ volatile("cli; hlt");
}
