#include "tunix_libc.h"

static char *const shell_environment[] = {
    "HOME=/home",
    "PATH=/bin:/usr/bin",
    "SHELL=/bin/bash",
    "TERM=tunix-256color",
    "TERMINFO=/usr/share/terminfo",
    "LANG=C.UTF-8",
    "USER=root",
    "LOGNAME=root",
    0
};

static int read_console_keymap(char *name, size_t capacity) {
    int fd = t_open("/etc/vconsole.conf", T_O_RDONLY, 0);
    if (fd < 0) return -1;
    char buffer[256];
    long amount = t_read(fd, buffer, sizeof(buffer) - 1U);
    t_close(fd);
    if (amount <= 0) return -1;
    buffer[amount] = 0;
    const char prefix[] = "KEYMAP=";
    for (size_t at = 0; at < (size_t)amount;) {
        while (at < (size_t)amount && (buffer[at] == '\n' || buffer[at] == '\r')) at++;
        size_t line = at;
        while (at < (size_t)amount && buffer[at] != '\n' && buffer[at] != '\r') at++;
        size_t length = at - line;
        if (length > sizeof(prefix) - 1U &&
            t_strncmp(buffer + line, prefix, sizeof(prefix) - 1U) == 0) {
            size_t value = line + sizeof(prefix) - 1U;
            size_t written = 0;
            while (value < line + length && written + 1U < capacity) {
                char c = buffer[value++];
                if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                      (c >= '0' && c <= '9') || c == '_' || c == '-')) return -1;
                name[written++] = c;
            }
            if (!written || value != line + length) return -1;
            name[written] = 0;
            return 0;
        }
    }
    return -1;
}

static int load_console_keymap(void) {
    char keymap[32];
    if (read_console_keymap(keymap, sizeof(keymap)) != 0) return 0;
    long child = t_fork();
    if (child < 0) return -1;
    if (child == 0) {
        char *arguments[] = {"/bin/loadkeys", "-q", keymap, 0};
        t_execve(arguments[0], arguments, shell_environment);
        t_puterr("init: cannot execute /bin/loadkeys\n");
        t_exit(127);
    }
    int status = 0;
    while (t_waitpid(child, &status, 0) < 0) t_yield();
    return (status >> 8) & 0xff;
}

static int spawn(char *const argv[]) {
    long child = t_fork();
    if (child < 0) return -1;
    if (child == 0) {
        t_setpgid(0, 0);
        int pgid = (int)t_getpgrp();
        t_ioctl(0, T_TIOCSPGRP, &pgid);
        t_execve(argv[0], argv, shell_environment);
        t_puterr("init: cannot execute /bin/bash\n");
        t_exit(127);
    }

    int status = 0;
    while (t_waitpid(child, &status, 0) < 0) t_yield();
    return (status >> 8) & 0xff;
}

int main(int argc, char **argv, char **envp) {
    (void)argc;
    (void)argv;
    (void)envp;

    t_mkdir("/tmp", 0777);
    t_mkdir("/home", 0755);
    if (load_console_keymap() != 0)
        t_puterr("init: warning: configured console keymap could not be loaded\n");

    char *shell[] = {"/bin/bash", "--login", "-i", 0};
    for (;;) {
        int status = spawn(shell);
        if (status < 0) {
            t_puterr("init: failed to start GNU Bash\n");
            return 1;
        }
        t_sleep_ms(500);
    }
}
