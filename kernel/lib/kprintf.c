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

/*
 * Whether the log also goes to the screen.
 *
 * On for the whole boot, and the reason is machines with no serial port: a
 * kernel that stops between the console coming up and init running leaves
 * nothing behind but a cursor, and which line it stopped after is the entire
 * diagnosis. It costs a few thousand glyphs.
 *
 * Painting is the terminal's decision, not this one -- it draws only while the
 * console owns the framebuffer, so once the compositor has taken the display
 * these characters go to the log and nowhere else.
 */
static int klog_console_enabled = 1;
static int klog_console_busy;

void klog_console(int enabled) {
    klog_console_enabled = enabled;
}

extern void terminal_print(const char *);
extern int terminal_ready(void);
extern void terminal_paint_lock_reset(void);

static void emit_char(char c) {
    klog_store_char(c);
    serial_write_char(c);

    if (!klog_console_enabled || klog_console_busy || !terminal_ready()) return;
    /* The terminal takes strings and this takes characters; the pair is the
       shortest thing that is both. The flag is belt and braces: nothing under
       terminal_print() logs, and if that ever changes this is why it will not
       be a stack overflow. */
    klog_console_busy = 1;
    char pair[2] = {c, 0};
    terminal_print(pair);
    klog_console_busy = 0;
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

/*
 * A message at a time.
 *
 * The terminal serialises characters on its own, which keeps the screen from
 * being corrupted but not from being unreadable: two processors printing at
 * once produce one line with both messages spliced into it, a character each.
 * This is the lock that makes a kprintf() atomic, and it is why the trace a
 * fault handler prints is legible at all.
 *
 * Interrupts go off with it for the usual reason: kprintf() is reachable from
 * an interrupt handler and a processor that took one here would wait for
 * itself.
 */
static volatile int log_lock;

static uint64_t log_acquire(void) {
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(flags) :: "memory");
    while (__atomic_test_and_set(&log_lock, __ATOMIC_ACQUIRE))
        __asm__ volatile("pause");
    return flags;
}

static void log_release(uint64_t flags) {
    __atomic_clear(&log_lock, __ATOMIC_RELEASE);
    if (flags & 0x200ULL) __asm__ volatile("sti");
}

void kprintf(const char *fmt, ...) {
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
}

/*
 * The tail of the log, on the terminal.
 *
 * A panic is the one moment the serial port is not enough. On real hardware
 * there is usually nothing attached to it, and the reason the kernel stopped
 * is never the panic line itself -- it is the twenty lines above it, which
 * until now only existed somewhere nobody was looking.
 */
void klog_print_tail(unsigned lines) {
    if (!klog_count) return;
    size_t start = klog_count;
    unsigned seen = 0;
    while (start > 0) {
        size_t index = (klog_head + start - 1U) % KLOG_CAPACITY;
        if (klog_buffer[index] == '\n' && ++seen > lines) break;
        start--;
    }
    /* One character at a time, because the log is a ring and the terminal
       takes strings: a pair is the shortest thing that is both. */
    char pair[2] = {0, 0};
    for (size_t at = start; at < klog_count; at++) {
        pair[0] = klog_buffer[(klog_head + at) % KLOG_CAPACITY];
        terminal_print(pair);
    }
}

#define PANIC_LOG_LINES 24U

void panic(const char *msg) {
    /* Both locks by force. The processor that held either of them may be the
       one that just went wrong, and a panic that waits for it says nothing. */
    __atomic_clear(&log_lock, __ATOMIC_RELEASE);
    terminal_paint_lock_reset();
    kprintf("PANIC: %s\n", msg);
    terminal_print("\n\n--- kernel log ---\n");
    klog_print_tail(PANIC_LOG_LINES);
    terminal_print("\n\n*** KERNEL PANIC ***\n");
    terminal_print(msg);
    while (1) __asm__ volatile("cli; hlt");
}
