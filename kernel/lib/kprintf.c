#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include "../include/cpu.h"
#include "../include/klog.h"
#include "../include/smp.h"
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

static int klog_console_enabled = 1;
static int klog_console_busy;

void klog_console(int enabled) {
    klog_console_enabled = enabled;
}

extern void terminal_print(const char *);
extern int terminal_ready(void);
extern void terminal_paint_begin(void);
extern void terminal_paint_end(void);
extern void terminal_paint_lock_reset(void);
struct terminal_screen;
extern struct terminal_screen *terminal_screen_active(void);
extern void terminal_set_sgr_sequence(struct terminal_screen *screen,
                                      const unsigned *codes, unsigned count);

static void panic_sgr(const unsigned *codes, unsigned count) {
    terminal_set_sgr_sequence(terminal_screen_active(), codes, count);
}

static void emit_char(char c) {
    klog_store_char(c);
    serial_write_char(c);

    if (!klog_console_enabled || klog_console_busy || !terminal_ready()) return;
    klog_console_busy = 1;
    char pair[2] = {c, 0};
    terminal_print(pair);
    klog_console_busy = 0;
}

static volatile int log_lock;

static uint64_t log_acquire(void) {
    uint64_t flags = cpu_irq_save();
    while (__atomic_test_and_set(&log_lock, __ATOMIC_ACQUIRE)) {
        smp_service_flush();
        cpu_relax();
    }
    return flags;
}

static void log_release(uint64_t flags) {
    __atomic_clear(&log_lock, __ATOMIC_RELEASE);
    cpu_irq_restore(flags);
}

static int paint_begin(void) {
    if (!terminal_ready()) return 0;
    terminal_paint_begin();
    return 1;
}

static void paint_end(int painting) {
    if (painting) terminal_paint_end();
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
    int painting = paint_begin();
    uint64_t flags = log_acquire();
    for (size_t i = 0; i < size; i++) emit_char(bytes[i]);
    log_release(flags);
    paint_end(painting);
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
    int painting = paint_begin();
    uint64_t flags = log_acquire();
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
    log_release(flags);
    paint_end(painting);
}

void klog_print_tail(unsigned lines) {
    if (!klog_count) return;
    size_t start = klog_count;
    unsigned seen = 0;
    while (start > 0) {
        size_t index = (klog_head + start - 1U) % KLOG_CAPACITY;
        if (klog_buffer[index] == '\n' && ++seen > lines) break;
        start--;
    }
    char pair[2] = {0, 0};
    for (size_t at = start; at < klog_count; at++) {
        pair[0] = klog_buffer[(klog_head + at) % KLOG_CAPACITY];
        terminal_print(pair);
    }
}

#define PANIC_LOG_LINES 24U

void panic(const char *msg) {
    __atomic_clear(&log_lock, __ATOMIC_RELEASE);
    terminal_paint_lock_reset();
    int painting = paint_begin();
    panic_sgr(NULL, 0);
    kprintf("PANIC: %s\n", msg);
    static const unsigned heading[] = {34U};
    static const unsigned alarm[] = {1U, 31U};
    panic_sgr(heading, 1U);
    terminal_print("\n\n--- kernel log ---\n");
    panic_sgr(NULL, 0);
    klog_print_tail(PANIC_LOG_LINES);
    panic_sgr(alarm, 2U);
    terminal_print("\n\n*** KERNEL PANIC ***\n");
    terminal_print(msg);
    paint_end(painting);
    cpu_halt_forever();
}
