#ifndef TUNIX_LIBC_H
#define TUNIX_LIBC_H

#include <stddef.h>
#include <stdint.h>

#define T_O_RDONLY 0
#define T_O_WRONLY 1
#define T_O_RDWR 2
#define T_O_CREAT 0100
#define T_O_NOCTTY 0400
#define T_O_TRUNC 01000
#define T_O_APPEND 02000
#define T_O_NONBLOCK 04000
#define T_O_DIRECTORY 0200000
#define T_AT_FDCWD (-100)
#define T_EAGAIN 11
#define T_EINTR 4
#define T_ENOENT 2
#define T_TCGETS 0x5401UL
#define T_TCSETS 0x5402UL
#define T_TIOCGPGRP 0x540FUL
#define T_TIOCSPGRP 0x5410UL
#define T_TIOCSCTTY 0x540EUL
#define T_TIOCGWINSZ 0x5413UL
#define T_TIOCSWINSZ 0x5414UL
#define T_TIOCGPTN 0x80045430UL
#define T_TIOCSPTLCK 0x40045431UL
#define T_PROT_READ 0x1
#define T_PROT_WRITE 0x2
#define T_PROT_EXEC 0x4
#define T_MAP_SHARED 0x01
#define T_MAP_PRIVATE 0x02
#define T_MAP_FIXED 0x10
#define T_MAP_ANONYMOUS 0x20
#define T_MAP_FAILED ((void *)(uintptr_t)-1)
#define T_POLLIN 0x0001
#define T_POLLOUT 0x0004
#define T_POLLERR 0x0008
#define T_POLLHUP 0x0010
#define T_POLLNVAL 0x0020
#define T_WNOHANG 1
#define T_SIGHUP 1
#define T_SIGTERM 15
#define T_SIGCONT 18
#define T_SIG_BLOCK 0
#define T_SIG_UNBLOCK 1
#define T_SIG_SETMASK 2
#define T_FUTEX_WAIT 0
#define T_FUTEX_WAKE 1
#define T_FUTEX_PRIVATE_FLAG 128
#define T_AF_UNIX 1
#define T_SOCK_STREAM 1


struct t_sockaddr_un {
    uint16_t family;
    char path[108];
};

struct t_pollfd {
    int32_t fd;
    int16_t events;
    int16_t revents;
};

struct t_utsname {
    char sysname[65];
    char nodename[65];
    char release[65];
    char version[65];
    char machine[65];
    char domainname[65];
};

struct t_winsize {
    uint16_t rows;
    uint16_t cols;
    uint16_t xpixel;
    uint16_t ypixel;
};

struct t_sigaction {
    uint64_t handler;
    uint64_t flags;
    uint64_t restorer;
    uint64_t mask;
};

struct t_timespec {
    int64_t tv_sec;
    int64_t tv_nsec;
};

struct t_linux_dirent64 {
    uint64_t ino;
    int64_t off;
    uint16_t reclen;
    uint8_t type;
    char name[];
} __attribute__((packed));

_Static_assert(offsetof(struct t_linux_dirent64, name) == 19U,
               "getdents64 ABI mismatch");

extern char **t_environ;

long t_syscall0(long n);
long t_syscall1(long n, long a1);
long t_syscall2(long n, long a1, long a2);
long t_syscall3(long n, long a1, long a2, long a3);
long t_syscall4(long n, long a1, long a2, long a3, long a4);
long t_syscall5(long n, long a1, long a2, long a3, long a4, long a5);
long t_syscall6(long n, long a1, long a2, long a3, long a4, long a5, long a6);

long t_read(int fd, void *buffer, size_t size);
long t_write(int fd, const void *buffer, size_t size);
int t_open(const char *path, int flags, int mode);
int t_close(int fd);
int t_poll(struct t_pollfd *fds, unsigned long count, int timeout_ms);
int t_socket(int domain, int type, int protocol);
int t_bind(int fd, const struct t_sockaddr_un *address, unsigned long length);
int t_listen(int fd, int backlog);
int t_accept(int fd);
int t_connect(int fd, const struct t_sockaddr_un *address, unsigned long length);
int t_socketpair(int domain, int type, int protocol, int fds[2]);
long t_lseek(int fd, long offset, int whence);
int t_pipe(int fds[2]);
int t_dup2(int oldfd, int newfd);
long t_fork(void);
int t_execve(const char *path, char *const argv[], char *const envp[]);
long t_waitpid(long pid, int *status, int options);
void t_exit(int status) __attribute__((noreturn));
long t_getpid(void);
long t_gettid(void);
long t_getppid(void);
int t_setpgid(long pid, long pgid);
long t_getpgrp(void);
long t_setsid(void);
int t_kill(long pid, int signal_number);
int t_sigaction(int signal_number, const struct t_sigaction *action, struct t_sigaction *old_action);
int t_sigprocmask(int how, const uint64_t *set, uint64_t *old_set);
int t_ioctl(int fd, unsigned long request, void *argument);
void *t_mmap(void *address, size_t length, int prot, int flags, int fd, uint64_t offset);
int t_munmap(void *address, size_t length);
int t_mprotect(void *address, size_t length, int prot);
int t_ftruncate(int fd, uint64_t length);
int t_chdir(const char *path);
char *t_getcwd(char *buffer, size_t size);
int t_mkdir(const char *path, int mode);
int t_unlink(const char *path);
long t_getdents64(int fd, void *buffer, size_t size);
int t_uname(struct t_utsname *name);
void t_yield(void);
int t_clock_gettime(struct t_timespec *time);
int t_nanosleep(const struct t_timespec *request, struct t_timespec *remaining);
int t_futex(uint32_t *address, int operation, uint32_t value, const struct t_timespec *timeout);
void t_sleep_ms(uint64_t milliseconds);

size_t t_strlen(const char *text);
int t_strcmp(const char *a, const char *b);
int t_strncmp(const char *a, const char *b, size_t amount);
char *t_strcpy(char *destination, const char *source);
char *t_strncpy(char *destination, const char *source, size_t amount);
void *t_memcpy(void *destination, const void *source, size_t amount);
void *t_memset(void *destination, int value, size_t amount);
char *t_strchr(const char *text, int value);
const char *t_basename(const char *path);
char *t_getenv(const char *name);
void t_puts(const char *text);
void t_puterr(const char *text);
void t_print_long(long value);
int t_read_retry(int fd, void *buffer, size_t size);

#endif
