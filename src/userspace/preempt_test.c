#include "tunix_libc.h"

#define SPIN_ITERATIONS 50000000ULL

static void log_line(const char *text) {
    t_puts(text);
    int fd = t_open("/dev/kmsg", T_O_WRONLY, 0);
    if (fd < 0) return;
    (void)t_write(fd, text, t_strlen(text));
    t_close(fd);
}

static void cpu_spin(void) {
    uint64_t iterations = SPIN_ITERATIONS;
    __asm__ volatile(
        "1:\n"
        "sub $1, %0\n"
        "jnz 1b\n"
        : "+r"(iterations)
        :
        : "cc", "memory");
}

static void close_pair(int pair[2]) {
    t_close(pair[0]);
    t_close(pair[1]);
}

int main(int argc, char **argv, char **envp) {
    (void)argc;
    (void)argv;
    (void)envp;

    int start_a[2];
    int start_b[2];
    int result[2];
    if (t_pipe(start_a) < 0 || t_pipe(start_b) < 0 || t_pipe(result) < 0) {
        log_line("SCHEDTEST: FAIL pipe setup\n");
        return 1;
    }

    long child_a = t_fork();
    if (child_a < 0) {
        log_line("SCHEDTEST: FAIL fork A\n");
        return 1;
    }
    if (child_a == 0) {
        char token;
        t_close(start_a[1]);
        close_pair(start_b);
        t_close(result[0]);
        if (t_read_retry(start_a[0], &token, 1) != 1) t_exit(2);
        cpu_spin();
        if (t_write(result[1], "A", 1) != 1) t_exit(3);
        t_exit(0);
    }

    long child_b = t_fork();
    if (child_b < 0) {
        log_line("SCHEDTEST: FAIL fork B\n");
        return 1;
    }
    if (child_b == 0) {
        char token;
        close_pair(start_a);
        t_close(start_b[1]);
        t_close(result[0]);
        if (t_read_retry(start_b[0], &token, 1) != 1) t_exit(4);
        if (t_write(result[1], "B", 1) != 1) t_exit(5);
        t_exit(0);
    }

    t_close(start_a[0]);
    t_close(start_b[0]);
    t_close(result[1]);
    if (t_write(start_a[1], "x", 1) != 1 ||
        t_write(start_b[1], "x", 1) != 1) {
        log_line("SCHEDTEST: FAIL start signal\n");
        return 1;
    }
    t_close(start_a[1]);
    t_close(start_b[1]);

    char first = 0;
    if (t_read_retry(result[0], &first, 1) != 1) {
        log_line("SCHEDTEST: FAIL result read\n");
        return 1;
    }

    int status_a = 0;
    int status_b = 0;
    while (t_waitpid(child_a, &status_a, 0) < 0) t_yield();
    while (t_waitpid(child_b, &status_b, 0) < 0) t_yield();
    t_close(result[0]);

    if (first == 'B' && ((status_a >> 8) & 0xff) == 0 &&
        ((status_b >> 8) & 0xff) == 0) {
        log_line("SCHEDTEST: PASS timer preemption observed\n");
        return 0;
    }

    log_line("SCHEDTEST: FAIL cooperative ordering observed\n");
    return 1;
}
