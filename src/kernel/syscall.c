#include <stddef.h>
#include <stdint.h>
#include "include/file.h"
#include "include/eventfd.h"
#include "include/timerfd.h"
#include "include/epoll.h"
#include "include/inotify.h"
#include "include/framebuffer.h"
#include "include/input.h"
#include "include/heap.h"
#include "include/kstring.h"
#include "include/pipe.h"
#include "include/pty.h"
#include "include/pmm.h"
#include "include/process.h"
#include "include/random.h"
#include "include/signal.h"
#include "include/syscall.h"
#include "include/ext2.h"
#include "include/time.h"
#include "include/tty.h"
#include "include/usercopy.h"
#include "include/vfs.h"
#include "include/terminal.h"
#include "include/vmm.h"
#include "include/unix_socket.h"
#include "include/net/inet_socket.h"
#include "include/net/netlink.h"
#include "include/net/net.h"

extern void kprintf(const char *fmt, ...);

#if TUNIX_DEBUG_LOGS
#define KDEBUG(...) kprintf(__VA_ARGS__)
#else
#define KDEBUG(...) do { } while (0)
#endif

_Static_assert(sizeof(struct syscall_frame) == 144, "syscall frame/assembly ABI mismatch");
_Static_assert(offsetof(struct syscall_frame, rax) == 96, "syscall frame rax offset mismatch");
_Static_assert(offsetof(struct syscall_frame, rcx) == 104, "syscall frame rcx offset mismatch");
_Static_assert(offsetof(struct syscall_frame, r11) == 112, "syscall frame r11 offset mismatch");
_Static_assert(offsetof(struct syscall_frame, user_rip) == 120, "syscall frame rip offset mismatch");
_Static_assert(offsetof(struct syscall_frame, user_rsp) == 136, "syscall frame rsp offset mismatch");

#define USER_BRK_LIMIT 0x00005F0000000000ULL

#define SYS_READ 0
#define SYS_WRITE 1
#define SYS_OPEN 2
#define SYS_CLOSE 3
#define SYS_STAT 4
#define SYS_FSTAT 5
#define SYS_LSTAT 6
#define SYS_POLL 7
#define SYS_LSEEK 8
#define SYS_MMAP 9
#define SYS_MPROTECT 10
#define SYS_MUNMAP 11
#define SYS_BRK 12
#define SYS_MSYNC 26
#define SYS_MADVISE 28
#define SYS_FADVISE64 221
#define SYS_RT_SIGACTION 13
#define SYS_RT_SIGPROCMASK 14
#define SYS_RT_SIGRETURN 15
#define SYS_IOCTL 16
#define SYS_PREAD64 17
#define SYS_PWRITE64 18
#define SYS_READV 19
#define SYS_WRITEV 20
#define SYS_ACCESS 21
#define SYS_PIPE 22
#define SYS_SELECT 23
#define SYS_SCHED_YIELD 24
#define SYS_EPOLL_CREATE 213
#define SYS_DUP 32
#define SYS_DUP2 33
#define SYS_NANOSLEEP 35
#define SYS_GETITIMER 36
#define SYS_ALARM 37
#define SYS_SETITIMER 38
#define SYS_GETPID 39
#define SYS_SOCKET 41
#define SYS_CONNECT 42
#define SYS_ACCEPT 43
#define SYS_SENDTO 44
#define SYS_RECVFROM 45
#define SYS_SENDMSG 46
#define SYS_RECVMSG 47
#define SYS_SHUTDOWN 48
#define SYS_BIND 49
#define SYS_LISTEN 50
#define SYS_GETSOCKNAME 51
#define SYS_GETPEERNAME 52
#define SYS_SOCKETPAIR 53
#define SYS_SETSOCKOPT 54
#define SYS_GETSOCKOPT 55
#define SYS_CLONE 56
#define SYS_FORK 57
#define SYS_VFORK 58
#define SYS_EXECVE 59
#define SYS_EXIT 60
#define SYS_WAIT4 61
#define SYS_KILL 62
#define SYS_UNAME 63
#define SYS_FCNTL 72
#define SYS_FSYNC 74
#define SYS_FDATASYNC 75
#define SYS_STATFS 137
#define SYS_FSTATFS 138
#define SYS_SYNC 162
#define SYS_SYNCFS 306
#define SYS_FTRUNCATE 77
#define SYS_GETCWD 79
#define SYS_CHDIR 80
#define SYS_FCHDIR 81
#define SYS_RENAME 82
#define SYS_MKDIR 83
#define SYS_RMDIR 84
#define SYS_UNLINK 87
#define SYS_SYMLINK 88
#define SYS_READLINK 89
#define SYS_CHMOD 90
#define SYS_FCHMOD 91
#define SYS_UMASK 95
#define SYS_GETTIMEOFDAY 96
#define SYS_GETRLIMIT 97
#define SYS_GETRUSAGE 98
#define SYS_GETUID 102
#define SYS_GETGID 104
#define SYS_GETEUID 107
#define SYS_GETEGID 108
#define SYS_GETGROUPS 115
#define SYS_SETPGID 109
#define SYS_GETPPID 110
#define SYS_GETPGRP 111
#define SYS_SETSID 112
#define SYS_GETPGID 121
#define SYS_GETSID 124
#define SYS_SIGALTSTACK 131
#define SYS_ARCH_PRCTL 158
#define SYS_PRCTL 157
#define SYS_GETTID 186
#define SYS_FUTEX 202
#define SYS_SET_TID_ADDRESS 218
#define SYS_CLOCK_GETTIME 228
#define SYS_CLOCK_GETRES 229
#define SYS_CLOCK_NANOSLEEP 230
#define SYS_EPOLL_WAIT 232
#define SYS_EPOLL_CTL 233
#define SYS_EXIT_GROUP 231
#define SYS_TKILL 200
#define SYS_TGKILL 234
#define SYS_INOTIFY_INIT 253
#define SYS_INOTIFY_ADD_WATCH 254
#define SYS_INOTIFY_RM_WATCH 255
#define SYS_OPENAT 257
#define SYS_MKDIRAT 258
#define SYS_NEWFSTATAT 262
#define SYS_UNLINKAT 263
#define SYS_RENAMEAT 264
#define SYS_SYMLINKAT 266
#define SYS_READLINKAT 267
#define SYS_FCHMODAT 268
#define SYS_FACCESSAT 269
#define SYS_PSELECT6 270
#define SYS_PPOLL 271
#define SYS_UTIMENSAT 280
#define SYS_EPOLL_PWAIT 281
#define SYS_TIMERFD_CREATE 283
#define SYS_EVENTFD 284
#define SYS_TIMERFD_SETTIME 286
#define SYS_TIMERFD_GETTIME 287
#define SYS_SET_ROBUST_LIST 273
#define SYS_GET_ROBUST_LIST 274
#define SYS_ACCEPT4 288
#define SYS_EVENTFD2 290
#define SYS_EPOLL_CREATE1 291
#define SYS_PIPE2 293
#define SYS_INOTIFY_INIT1 294
#define SYS_DUP3 292
#define SYS_PRLIMIT64 302
#define SYS_RENAMEAT2 316
#define SYS_GETRANDOM 318
#define SYS_STATX 332
#define SYS_RSEQ 334
#define SYS_CLONE3 435
#define SYS_CLOSE_RANGE 436
#define SYS_FACCESSAT2 439
#define SYS_GETDENTS64 217

#define AT_FDCWD (-100)
#define AT_SYMLINK_NOFOLLOW 0x100
#define AT_EACCESS 0x200
#define AT_EMPTY_PATH 0x1000
/* Linux accepts and ignores this whenever there is nothing to automount, which
   is always the case here. coreutils passes it on every fstatat. */
#define AT_NO_AUTOMOUNT 0x800
#define AT_REMOVEDIR 0x200
#define AT_SYMLINK_FOLLOW 0x400

#define O_ACCMODE 3
#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR 2
#define O_CREAT 0100
#define O_EXCL 0200
#define O_NOCTTY 0400
#define O_TRUNC 01000
#define O_APPEND 02000
#define O_NONBLOCK 04000
#define O_DSYNC 010000
#define O_ASYNC 020000
#define O_DIRECT 040000
#define O_LARGEFILE 0100000
#define O_DIRECTORY 0200000
#define O_NOFOLLOW 0400000
#define O_CLOEXEC 02000000
#define O_NOATIME 01000000
#define O_PATH 010000000
#define O_TMPFILE 020200000
#define O_SYNC 04010000

#define MSG_DONTWAIT 0x40
#define MSG_CTRUNC 0x08
#define MSG_CMSG_CLOEXEC 0x40000000
#define SOCK_NONBLOCK O_NONBLOCK
#define SOCK_CLOEXEC O_CLOEXEC
#define FD_CLOEXEC 1
#define EFD_SEMAPHORE 1
#define EFD_NONBLOCK O_NONBLOCK
#define EFD_CLOEXEC O_CLOEXEC
#define TFD_NONBLOCK O_NONBLOCK
#define TFD_CLOEXEC O_CLOEXEC
#define TFD_TIMER_ABSTIME 1
#define EPOLL_CLOEXEC O_CLOEXEC
#define EPOLL_CTL_ADD 1
#define EPOLL_CTL_DEL 2
#define EPOLL_CTL_MOD 3
#define SOL_SOCKET 1
#define SCM_RIGHTS 1
#define SCM_CREDENTIALS 2
#define SO_TYPE 3
#define SO_ERROR 4
#define SO_PASSCRED 16
#define SO_PEERCRED 17
#define SO_ACCEPTCONN 30
#define IN_NONBLOCK O_NONBLOCK
#define IN_CLOEXEC O_CLOEXEC
#define SIOCGIFCONF 0x8912U

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

#define PROT_WRITE 0x2
#define PROT_EXEC 0x4
#define MAP_SHARED 0x01
#define MAP_PRIVATE 0x02
#define MAP_FIXED 0x10
#define MAP_ANONYMOUS 0x20
#define MAP_FIXED_NOREPLACE 0x100000

#define F_DUPFD 0
#define F_GETFD 1
#define F_SETFD 2
#define F_GETFL 3
#define F_SETFL 4
#define F_DUPFD_CLOEXEC 1030

#define POLLIN   0x0001
#define POLLOUT  0x0004
#define POLLERR  0x0008
#define POLLHUP  0x0010
#define POLLNVAL 0x0020

#define CLONE_VM              0x00000100ULL
#define CLONE_FS              0x00000200ULL
#define CLONE_FILES           0x00000400ULL
#define CLONE_SIGHAND         0x00000800ULL
#define CLONE_VFORK           0x00004000ULL
#define CLONE_PARENT          0x00008000ULL
#define CLONE_THREAD          0x00010000ULL
#define CLONE_SYSVSEM         0x00040000ULL
#define CLONE_SETTLS          0x00080000ULL
#define CLONE_PARENT_SETTID   0x00100000ULL
#define CLONE_CHILD_CLEARTID  0x00200000ULL
#define CLONE_DETACHED        0x00400000ULL
#define CLONE_CHILD_SETTID    0x01000000ULL
#define CLONE_FORK_METADATA_FLAGS \
    (CLONE_PARENT_SETTID | CLONE_CHILD_CLEARTID | CLONE_CHILD_SETTID)
#define CLONE_FORK_REJECT_FLAGS \
    (CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND | CLONE_VFORK | \
     CLONE_PARENT | CLONE_THREAD | CLONE_SYSVSEM | CLONE_SETTLS)

struct linux_clone_args {
    uint64_t flags;
    uint64_t pidfd;
    uint64_t child_tid;
    uint64_t parent_tid;
    uint64_t exit_signal;
    uint64_t stack;
    uint64_t stack_size;
    uint64_t tls;
    uint64_t set_tid;
    uint64_t set_tid_size;
    uint64_t cgroup;
};

#define ARCH_SET_FS 0x1002
#define ARCH_GET_FS 0x1003

#define PR_SET_PDEATHSIG 1
#define PR_GET_PDEATHSIG 2
#define PR_GET_DUMPABLE 3
#define PR_SET_DUMPABLE 4
#define PR_GET_KEEPCAPS 7
#define PR_SET_KEEPCAPS 8
#define PR_SET_NAME 15
#define PR_GET_NAME 16
#define PR_GET_SECCOMP 21
#define PR_CAPBSET_READ 23
#define PR_CAPBSET_DROP 24
#define PR_GET_SECUREBITS 27
#define PR_SET_SECUREBITS 28
#define PR_SET_TIMERSLACK 29
#define PR_GET_TIMERSLACK 30
#define PR_SET_PTRACER 0x59616d61
#define PR_SET_CHILD_SUBREAPER 36
#define PR_GET_CHILD_SUBREAPER 37
#define PR_SET_NO_NEW_PRIVS 38
#define PR_GET_NO_NEW_PRIVS 39
#define PR_GET_TID_ADDRESS 40
#define PR_SET_THP_DISABLE 41
#define PR_GET_THP_DISABLE 42
#define PR_CAP_AMBIENT 47
#define PR_CAP_AMBIENT_IS_SET 1
#define PR_CAP_AMBIENT_RAISE 2
#define PR_CAP_AMBIENT_LOWER 3
#define PR_CAP_AMBIENT_CLEAR_ALL 4


#define EPERM 1
#define E2BIG 7
#define ENOEXEC 8
#define ENOENT 2
#define ESRCH 3
#define EINTR 4
#define EIO 5
#define ENXIO 6
#define EBADF 9
#define ECHILD 10
#define EAGAIN 11
#define ENOMEM 12
#define EACCES 13
#define EFAULT 14
#define EBUSY 16
#define EEXIST 17
#define ENODEV 19
#define ENOTDIR 20
#define EISDIR 21
#define EINVAL 22
#define EMFILE 24
#define ENOTTY 25
#define ESPIPE 29
#define EROFS 30
#define EPIPE 32
#define ENOSYS 38
#define ENOTEMPTY 39
#define ELOOP 40
#define EOPNOTSUPP 95
#define ENOSPC 28
#define ENAMETOOLONG 36
#define ERANGE 34
#define ETIMEDOUT 110
#define EMSGSIZE 90
#define EPROTONOSUPPORT 93
#define EAFNOSUPPORT 97
#define EADDRINUSE 98
#define EADDRNOTAVAIL 99
#define ENETDOWN 100
#define ENOTCONN 107
#define EDESTADDRREQ 89
#define ENOTSOCK 88
#define EINPROGRESS 115

#define FUTEX_WAIT 0
#define FUTEX_WAKE 1
#define FUTEX_PRIVATE_FLAG 128
#define FUTEX_CMD_MASK 0x7F

#define MAX_EXEC_ITEMS 64
#define MAX_EXEC_STRING 256

struct linux_timespec {
    int64_t tv_sec;
    int64_t tv_nsec;
};

struct linux_timeval {
    int64_t tv_sec;
    int64_t tv_usec;
};

struct linux_pollfd {
    int32_t fd;
    int16_t events;
    int16_t revents;
};

struct linux_fd_set {
    uint64_t words[16];
};

struct linux_rlimit {
    uint64_t rlim_cur;
    uint64_t rlim_max;
};

struct linux_stat {
    uint64_t st_dev;
    uint64_t st_ino;
    uint64_t st_nlink;
    uint32_t st_mode;
    uint32_t st_uid;
    uint32_t st_gid;
    uint32_t __pad0;
    uint64_t st_rdev;
    int64_t st_size;
    int64_t st_blksize;
    int64_t st_blocks;
    struct linux_timespec st_atim;
    struct linux_timespec st_mtim;
    struct linux_timespec st_ctim;
    int64_t __glibc_reserved[3];
};

/* x86_64 layout: every field is 8 bytes, 120 bytes total. musl reads this
   directly and derives statvfs from it. */
struct linux_statfs {
    uint64_t f_type;
    uint64_t f_bsize;
    uint64_t f_blocks;
    uint64_t f_bfree;
    uint64_t f_bavail;
    uint64_t f_files;
    uint64_t f_ffree;
    int32_t f_fsid[2];
    uint64_t f_namelen;
    uint64_t f_frsize;
    uint64_t f_flags;
    uint64_t f_spare[4];
};

typedef char linux_statfs_size_check[(sizeof(struct linux_statfs) == 120) ? 1 : -1];

struct linux_statx_timestamp {
    int64_t tv_sec;
    uint32_t tv_nsec;
    int32_t __reserved;
};

struct linux_statx {
    uint32_t stx_mask;
    uint32_t stx_blksize;
    uint64_t stx_attributes;
    uint32_t stx_nlink;
    uint32_t stx_uid;
    uint32_t stx_gid;
    uint16_t stx_mode;
    uint16_t __spare0[1];
    uint64_t stx_ino;
    uint64_t stx_size;
    uint64_t stx_blocks;
    uint64_t stx_attributes_mask;
    struct linux_statx_timestamp stx_atime;
    struct linux_statx_timestamp stx_btime;
    struct linux_statx_timestamp stx_ctime;
    struct linux_statx_timestamp stx_mtime;
    uint32_t stx_rdev_major;
    uint32_t stx_rdev_minor;
    uint32_t stx_dev_major;
    uint32_t stx_dev_minor;
    uint64_t stx_mnt_id;
    uint32_t stx_dio_mem_align;
    uint32_t stx_dio_offset_align;
    uint64_t stx_subvol;
    uint32_t stx_atomic_write_unit_min;
    uint32_t stx_atomic_write_unit_max;
    uint32_t stx_atomic_write_segments_max;
    uint32_t __spare1[1];
    uint64_t __spare2[9];
};

typedef char linux_statx_size_check[(sizeof(struct linux_statx) == 256) ? 1 : -1];

#define STATX_BASIC_STATS 0x7FFU
/* AT_STATX_FORCE_SYNC | AT_STATX_DONT_SYNC: cache-coherency hints with nothing
   to do here, so they are accepted and ignored. */
#define AT_STATX_SYNC_TYPE 0x6000

#define EXT2_SUPER_MAGIC 0xEF53U
#define PROC_SUPER_MAGIC 0x9FA0U
#define TMPFS_MAGIC 0x01021994U

struct linux_iovec {
    uint64_t base;
    uint64_t length;
};

struct linux_sigaltstack {
    uint64_t sp;
    int32_t flags;
    uint32_t __pad;
    uint64_t size;
};

_Static_assert(sizeof(struct linux_sigaltstack) == 24, "Linux x86_64 sigaltstack ABI mismatch");

struct linux_msghdr {
    uint64_t name;
    uint32_t name_length;
    uint32_t __pad0;
    uint64_t iov;
    uint64_t iov_length;
    uint64_t control;
    uint64_t control_length;
    int32_t flags;
    uint32_t __pad1;
};

_Static_assert(sizeof(struct linux_msghdr) == 56, "Linux x86_64 msghdr ABI mismatch");

struct linux_cmsghdr {
    uint64_t length;
    int32_t level;
    int32_t type;
};

struct linux_ucred {
    int32_t pid;
    uint32_t uid;
    uint32_t gid;
};

_Static_assert(sizeof(struct linux_cmsghdr) == 16, "Linux x86_64 cmsghdr ABI mismatch");

static struct file *file_from_fd(int fd);
static int install_new_file(struct file *file, int cloexec);

static size_t cmsg_align(size_t value) {
    return (value + sizeof(uint64_t) - 1U) & ~(sizeof(uint64_t) - 1U);
}

struct linux_ifconf {
    int32_t length;
    uint32_t __pad;
    uint64_t buffer;
};

struct linux_ifreq {
    char name[16];
    uint8_t value[24];
};

_Static_assert(sizeof(struct linux_ifconf) == 16, "Linux x86_64 ifconf ABI mismatch");
_Static_assert(sizeof(struct linux_ifreq) == 40, "Linux x86_64 ifreq ABI mismatch");

struct linux_utsname {
    char sysname[65];
    char nodename[65];
    char release[65];
    char version[65];
    char machine[65];
    char domainname[65];
};

struct linux_winsize {
    uint16_t rows;
    uint16_t cols;
    uint16_t xpixel;
    uint16_t ypixel;
};

struct exec_arguments {
    char argv_storage[MAX_EXEC_ITEMS][MAX_EXEC_STRING];
    char env_storage[MAX_EXEC_ITEMS][MAX_EXEC_STRING];
    const char *argv[MAX_EXEC_ITEMS + 1];
    const char *envp[MAX_EXEC_ITEMS + 1];
};

extern void syscall_entry(void);

static int nx_enabled;

static inline void wrmsr(uint32_t msr, uint64_t value) {
    uint32_t low = (uint32_t)value;
    uint32_t high = (uint32_t)(value >> 32);
    __asm__ volatile("wrmsr" : : "c"(msr), "a"(low), "d"(high));
}

static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t low, high;
    __asm__ volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
    return ((uint64_t)high << 32) | low;
}

static inline uint64_t align_up(uint64_t value, uint64_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

void syscall_init(void) {
    uint32_t eax, ebx, ecx, edx;
    __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0x80000000U), "c"(0));
    nx_enabled = 0;
    if (eax >= 0x80000001U) {
        __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0x80000001U), "c"(0));
        nx_enabled = (edx & (1U << 20)) != 0;
    }
    uint64_t efer = rdmsr(0xC0000080);
    efer |= 1ULL;
    if (nx_enabled) efer |= 1ULL << 11;
    wrmsr(0xC0000080, efer);
    wrmsr(0xC0000081, ((uint64_t)0x10 << 48) | ((uint64_t)0x08 << 32));
    wrmsr(0xC0000082, (uint64_t)syscall_entry);
    wrmsr(0xC0000084, 0x200ULL | 0x400ULL);
}

static int64_t sys_write(int fd, uint64_t user_buffer, size_t length) {
    struct process *process = process_current();
    if (!process || fd < 0 || fd >= PROCESS_MAX_FDS || !process->fds[fd]) return -EBADF;
    uint8_t buffer[4096];
    size_t completed = 0;
    while (completed < length) {
        size_t chunk = length - completed;
        if (chunk > sizeof(buffer)) chunk = sizeof(buffer);
        if (copy_from_user(buffer, user_buffer + completed, chunk) != 0) return completed ? (int64_t)completed : -EFAULT;
        int64_t written = file_write(process->fds[fd], chunk, buffer);
        if (written == -EPIPE) {
            (void)process_send_signal((int64_t)process->pid, SIGPIPE);
            return completed ? (int64_t)completed : -EPIPE;
        }
        if (written < 0) return completed ? (int64_t)completed : written;
        completed += (size_t)written;
        if ((size_t)written < chunk) break;
    }
    return (int64_t)completed;
}

static int64_t sys_read(int fd, uint64_t user_buffer, size_t length) {
    struct process *process = process_current();
    if (!process || fd < 0 || fd >= PROCESS_MAX_FDS || !process->fds[fd]) return -EBADF;
    uint8_t buffer[4096];
    size_t completed = 0;
    while (completed < length) {
        size_t chunk = length - completed;
        if (chunk > sizeof(buffer)) chunk = sizeof(buffer);
        int64_t amount = file_read(process->fds[fd], chunk, buffer);
        if (amount < 0) return completed ? (int64_t)completed : amount;
        if (amount == 0) break;
        if (copy_to_user(user_buffer + completed, buffer, (size_t)amount) != 0) return completed ? (int64_t)completed : -EFAULT;
        completed += (size_t)amount;
        if ((size_t)amount < chunk) break;
    }
    return (int64_t)completed;
}

static int file_read_ready(struct file *file) {
    if (!file) return -1;
    return (file_poll_events(file, POLLIN) & (POLLIN | POLLHUP | POLLERR)) != 0;
}

static int file_write_ready(struct file *file) {
    if (!file) return -1;
    return (file_poll_events(file, POLLOUT) & (POLLOUT | POLLERR)) != 0;
}

static void clear_io_wait(struct process *process) {
    if (!process) return;
    process->io_wait_active = 0;
    process->io_wait_syscall = 0;
    process->io_wait_deadline_ns = 0;
}

static uint64_t saturating_add_u64(uint64_t left, uint64_t right) {
    if (UINT64_MAX - left < right) return UINT64_MAX;
    return left + right;
}

static int retry_io_wait(struct syscall_frame *frame, uint64_t syscall_number,
                         int64_t timeout_ns) {
    struct process *waiting = process_current();
    if (!waiting || !frame || timeout_ns == 0) return 0;

    uint64_t now = time_uptime_ns();
    if (!waiting->io_wait_active || waiting->io_wait_syscall != syscall_number) {
        waiting->io_wait_active = 1;
        waiting->io_wait_syscall = syscall_number;
        waiting->io_wait_deadline_ns = timeout_ns < 0 ? UINT64_MAX :
            saturating_add_u64(now, (uint64_t)timeout_ns);
    }

    if (waiting->io_wait_deadline_ns != UINT64_MAX &&
        now >= waiting->io_wait_deadline_ns) {
        clear_io_wait(waiting);
        return 0;
    }

    frame->user_rip -= 2U;
    frame->rax = syscall_number;
    waiting->syscall_rewound = 1;
    process_yield_from_syscall(frame);
    return 1;
}

static int64_t sys_poll_once(uint64_t user_fds, uint64_t count, int commit_empty) {
    if (count > PROCESS_MAX_FDS) return -EINVAL;
    struct linux_pollfd fds[PROCESS_MAX_FDS];
    size_t bytes = (size_t)count * sizeof(fds[0]);
    if (bytes && copy_from_user(fds, user_fds, bytes) != 0) return -EFAULT;

    int ready = 0;
    struct process *process = process_current();
    for (uint64_t i = 0; i < count; i++) {
        fds[i].revents = 0;
        int fd = fds[i].fd;
        if (fd < 0) continue;
        if (!process || fd >= PROCESS_MAX_FDS || !process->fds[fd]) {
            fds[i].revents = POLLNVAL;
            ready++;
            continue;
        }
        struct file *file = process->fds[fd];
        fds[i].revents = (int16_t)file_poll_events(file, (uint32_t)(uint16_t)fds[i].events);
        if (fds[i].revents) ready++;
    }
    if ((ready || commit_empty) && bytes && copy_to_user(user_fds, fds, bytes) != 0)
        return -EFAULT;
    return ready;
}

static int64_t timeout_ms_to_ns(int timeout_ms) {
    if (timeout_ms < 0) return -1;
    return (int64_t)timeout_ms * 1000000LL;
}

static int64_t read_timespec_timeout_ns(uint64_t user_timeout) {
    if (!user_timeout) return -1;
    struct linux_timespec timeout;
    if (copy_from_user(&timeout, user_timeout, sizeof(timeout)) != 0) return -EFAULT;
    if (timeout.tv_sec < 0 || timeout.tv_nsec < 0 || timeout.tv_nsec >= 1000000000LL)
        return -EINVAL;
    if ((uint64_t)timeout.tv_sec > (uint64_t)INT64_MAX / 1000000000ULL)
        return INT64_MAX;
    uint64_t value = (uint64_t)timeout.tv_sec * 1000000000ULL +
                     (uint64_t)timeout.tv_nsec;
    return value > (uint64_t)INT64_MAX ? INT64_MAX : (int64_t)value;
}

static int64_t read_timeval_timeout_ns(uint64_t user_timeout) {
    if (!user_timeout) return -1;
    struct linux_timeval timeout;
    if (copy_from_user(&timeout, user_timeout, sizeof(timeout)) != 0) return -EFAULT;
    if (timeout.tv_sec < 0 || timeout.tv_usec < 0 || timeout.tv_usec >= 1000000LL)
        return -EINVAL;
    if ((uint64_t)timeout.tv_sec > (uint64_t)INT64_MAX / 1000000000ULL)
        return INT64_MAX;
    uint64_t value = (uint64_t)timeout.tv_sec * 1000000000ULL +
                     (uint64_t)timeout.tv_usec * 1000ULL;
    return value > (uint64_t)INT64_MAX ? INT64_MAX : (int64_t)value;
}

static int fd_set_test(const struct linux_fd_set *set, int fd) {
    return set && (set->words[(unsigned)fd / 64U] & (1ULL << ((unsigned)fd % 64U)));
}

static void fd_set_put(struct linux_fd_set *set, int fd) {
    set->words[(unsigned)fd / 64U] |= 1ULL << ((unsigned)fd % 64U);
}

static int64_t sys_select_once(int nfds, uint64_t user_read, uint64_t user_write,
                               uint64_t user_except, int commit_empty) {
    if (nfds < 0 || nfds > PROCESS_MAX_FDS) return -EINVAL;
    struct linux_fd_set requested_read, requested_write, result_read, result_write, result_except;
    memset(&requested_read, 0, sizeof(requested_read));
    memset(&requested_write, 0, sizeof(requested_write));
    if (user_read && copy_from_user(&requested_read, user_read, sizeof(requested_read)) != 0)
        return -EFAULT;
    if (user_write && copy_from_user(&requested_write, user_write, sizeof(requested_write)) != 0)
        return -EFAULT;

    memset(&result_read, 0, sizeof(result_read));
    memset(&result_write, 0, sizeof(result_write));
    memset(&result_except, 0, sizeof(result_except));
    int ready = 0;
    struct process *process = process_current();
    for (int fd = 0; fd < nfds; fd++) {
        int requested = fd_set_test(&requested_read, fd) || fd_set_test(&requested_write, fd);
        if (!requested) continue;
        if (!process || !process->fds[fd]) return -EBADF;
        int this_ready = 0;
        if (fd_set_test(&requested_read, fd) && file_read_ready(process->fds[fd]) > 0) {
            fd_set_put(&result_read, fd);
            this_ready = 1;
        }
        if (fd_set_test(&requested_write, fd) && file_write_ready(process->fds[fd]) > 0) {
            fd_set_put(&result_write, fd);
            this_ready = 1;
        }
        if (this_ready) ready++;
    }
    if (ready || commit_empty) {
        if (user_read && copy_to_user(user_read, &result_read, sizeof(result_read)) != 0)
            return -EFAULT;
        if (user_write && copy_to_user(user_write, &result_write, sizeof(result_write)) != 0)
            return -EFAULT;
        if (user_except && copy_to_user(user_except, &result_except, sizeof(result_except)) != 0)
            return -EFAULT;
    }
    return ready;
}

static int normalize_path(struct vfs_node *base, const char *input, char output[256]) {
    if (!input || !input[0]) return -ENOENT;
    char combined[512];
    size_t at = 0;
    if (input[0] == '/') {
        combined[at++] = '/';
    } else {
        char base_path[256];
        if (vfs_node_path(base ? base : vfs_root, base_path, sizeof(base_path)) != 0) return -EINVAL;
        size_t length = strlen(base_path);
        if (length + 2 >= sizeof(combined)) return -ENAMETOOLONG;
        memcpy(combined, base_path, length);
        at = length;
        if (at == 0 || combined[at - 1] != '/') combined[at++] = '/';
    }
    size_t input_length = strlen(input);
    if (at + input_length + 1 > sizeof(combined)) return -ENAMETOOLONG;
    memcpy(combined + at, input, input_length + 1);

    size_t out = 0;
    output[out++] = '/';
    const char *cursor = combined;
    while (*cursor) {
        while (*cursor == '/') cursor++;
        if (!*cursor) break;
        char component[128];
        size_t length = 0;
        while (*cursor && *cursor != '/') {
            if (length + 1 >= sizeof(component)) return -ENAMETOOLONG;
            component[length++] = *cursor++;
        }
        component[length] = '\0';
        if (strcmp(component, ".") == 0) continue;
        if (strcmp(component, "..") == 0) {
            if (out > 1) {
                if (output[out - 1] == '/') out--;
                while (out > 1 && output[out - 1] != '/') out--;
            }
            continue;
        }
        if (out > 1 && output[out - 1] != '/') output[out++] = '/';
        if (out + length + 1 > 256) return -ENAMETOOLONG;
        memcpy(output + out, component, length);
        out += length;
    }
    if (out > 1 && output[out - 1] == '/') out--;
    output[out] = '\0';
    return 0;
}

static struct vfs_node *base_for_dirfd(int dirfd) {
    struct process *process = process_current();
    if (!process) return NULL;
    if (dirfd == AT_FDCWD) return process->cwd;
    if (dirfd < 0 || dirfd >= PROCESS_MAX_FDS || !process->fds[dirfd]) return NULL;
    struct file *file = process->fds[dirfd];
    if (file->kind != FILE_KIND_VFS || !file->node || (file->node->flags & 0xFFU) != VFS_DIRECTORY) return NULL;
    return file->node;
}

static int copy_path_at(int dirfd, uint64_t user_path, char output[256]) {
    char input[256];
    if (copy_string_from_user(input, sizeof(input), user_path) < 0) return -EFAULT;
    struct vfs_node *base = input[0] == '/' ? vfs_root : base_for_dirfd(dirfd);
    if (!base) return -EBADF;
    return normalize_path(base, input, output);
}

static uint32_t mode_after_umask(uint64_t mode) {
    return ((uint32_t)mode & 07777U) & ~process_get_umask();
}

static int64_t open_at(int dirfd, uint64_t user_path, uint64_t flags, uint64_t mode) {
    uint64_t supported = O_ACCMODE | O_CREAT | O_EXCL | O_NOCTTY | O_TRUNC |
                         O_APPEND | O_NONBLOCK | O_DSYNC | O_ASYNC | O_DIRECT |
                         O_LARGEFILE | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC | O_NOATIME |
                         O_PATH | O_SYNC;
    if ((flags & O_TMPFILE) == O_TMPFILE) return -EOPNOTSUPP;
    if (flags & ~supported) return -EINVAL;
    if ((flags & O_PATH) && (flags & (O_CREAT | O_EXCL | O_TRUNC))) return -EINVAL;

    char path[256];
    int path_status = copy_path_at(dirfd, user_path, path);
    if (path_status != 0) return path_status;

    struct vfs_node *node = (flags & O_NOFOLLOW) ? vfs_lookup_nofollow(path) : vfs_lookup(path);
    if (!node && (flags & O_CREAT)) {
        if (flags & O_DIRECTORY) return -EINVAL;
        node = vfs_create_file_node(path, mode_after_umask(mode));
        if (!node) return -ENOENT;
    } else if (!node) return -ENOENT;
    else if ((flags & O_CREAT) && (flags & O_EXCL)) return -EEXIST;

    uint32_t kind = node->flags & 0xFFU;
    if ((flags & O_NOFOLLOW) && kind == VFS_SYMLINK && !(flags & O_PATH)) return -ELOOP;
    if ((flags & O_DIRECTORY) && kind != VFS_DIRECTORY) return -ENOTDIR;
    if (!(flags & O_PATH) && kind == VFS_DIRECTORY && (flags & O_ACCMODE) != O_RDONLY) return -EISDIR;
    if (!(flags & O_PATH) && (node->flags & VFS_READONLY) &&
        ((flags & O_ACCMODE) != O_RDONLY || (flags & O_TRUNC))) return -EROFS;
    if (!(flags & O_PATH) && (flags & O_TRUNC) && kind == VFS_FILE &&
        vfs_truncate(node, 0) != 0) return -EIO;

    uint32_t status_flags = (uint32_t)(flags & ~O_CLOEXEC);
    struct file *file;
    if (strcmp(path, "/dev/ptmx") == 0) {
        file = pty_open_master(node, status_flags);
    } else if (strcmp(path, "/dev/tty") == 0) {
        if (process_current() && process_current()->controlling_pty) {
            file = pty_open_controlling(node, status_flags);
        } else {
            struct vfs_node *console = vfs_lookup("/dev/console");
            file = file_open_node(console, status_flags);
        }
    } else if (strncmp(path, "/dev/pts/", 9) == 0) {
        file = pty_open_slave(node, status_flags);
    } else {
        file = file_open_node(node, status_flags);
    }
    if (!file) {
        if (strcmp(path, "/dev/tty") == 0) return -ENXIO;
        if (strncmp(path, "/dev/pts/", 9) == 0) return -EIO;
        return -ENOMEM;
    }
    if (flags & O_APPEND) file->offset = node->length;
    int fd = process_install_file_flags(process_current(), file, 0,
        (flags & O_CLOEXEC) ? PROCESS_FD_CLOEXEC : 0);
    if (fd < 0) {
        file_unref(file);
        return -EMFILE;
    }
    return fd;
}

static int64_t sys_close(int fd) {
    return process_close_fd(process_current(), fd) == 0 ? 0 : -EBADF;
}

static int64_t sys_dup(int oldfd, int minimum, int cloexec) {
    struct process *process = process_current();
    if (!process || oldfd < 0 || oldfd >= PROCESS_MAX_FDS || !process->fds[oldfd]) return -EBADF;
    if (minimum < 0 || minimum >= PROCESS_MAX_FDS) return -EINVAL;
    file_ref(process->fds[oldfd]);
    int result = process_install_file_flags(process, process->fds[oldfd], minimum,
        cloexec ? PROCESS_FD_CLOEXEC : 0);
    if (result < 0) {
        file_unref(process->fds[oldfd]);
        return -EMFILE;
    }
    return result;
}

static int64_t sys_dup_to(int oldfd, int newfd, int cloexec, int reject_same) {
    struct process *process = process_current();
    if (!process || oldfd < 0 || oldfd >= PROCESS_MAX_FDS || !process->fds[oldfd]) return -EBADF;
    if (newfd < 0 || newfd >= PROCESS_MAX_FDS) return -EBADF;
    if (oldfd == newfd) return reject_same ? -EINVAL : newfd;
    if (process->fds[newfd]) process_close_fd(process, newfd);
    file_ref(process->fds[oldfd]);
    process->fds[newfd] = process->fds[oldfd];
    process->fd_flags[newfd] = cloexec ? PROCESS_FD_CLOEXEC : 0;
    return newfd;
}

static int64_t sys_pipe(uint64_t user_fds, int flags) {
    if (flags & ~(O_CLOEXEC | O_NONBLOCK)) return -EINVAL;
    struct file *read_end;
    struct file *write_end;
    if (pipe_create(&read_end, &write_end) != 0) return -EMFILE;
    read_end->flags = (uint32_t)(flags & O_NONBLOCK);
    write_end->flags = (uint32_t)(flags & O_NONBLOCK);
    struct process *process = process_current();
    uint8_t fd_flags = (flags & O_CLOEXEC) ? PROCESS_FD_CLOEXEC : 0;
    int read_fd = process_install_file_flags(process, read_end, 0, fd_flags);
    int write_fd = process_install_file_flags(process, write_end, 0, fd_flags);
    if (read_fd < 0 || write_fd < 0) {
        if (read_fd >= 0) process_close_fd(process, read_fd); else file_unref(read_end);
        if (write_fd >= 0) process_close_fd(process, write_fd); else file_unref(write_end);
        return -EMFILE;
    }
    int fds[2] = {read_fd, write_fd};
    if (copy_to_user(user_fds, fds, sizeof(fds)) != 0) {
        process_close_fd(process, read_fd);
        process_close_fd(process, write_fd);
        return -EFAULT;
    }
    return 0;
}

static struct unix_socket *socket_from_fd(int fd) {
    struct process *process = process_current();
    if (!process || fd < 0 || fd >= PROCESS_MAX_FDS || !process->fds[fd]) return NULL;
    struct file *file = process->fds[fd];
    return file->kind == FILE_KIND_SOCKET ? file->socket : NULL;
}

static struct inet_socket *inet_socket_from_fd(int fd) {
    struct process *process = process_current();
    if (!process || fd < 0 || fd >= PROCESS_MAX_FDS || !process->fds[fd]) return NULL;
    struct file *file = process->fds[fd];
    return file->kind == FILE_KIND_INET_SOCKET ? file->inet_socket : NULL;
}

static struct netlink_socket *netlink_socket_from_fd(int fd) {
    struct process *process = process_current();
    if (!process || fd < 0 || fd >= PROCESS_MAX_FDS || !process->fds[fd]) return NULL;
    struct file *file = process->fds[fd];
    return file->kind == FILE_KIND_NETLINK_SOCKET ? file->netlink_socket : NULL;
}

static int64_t sys_socket(int domain, int type, int protocol) {
    int base_type = type & 0xF;
    int type_flags = type & ~0xF;
    if (type_flags & ~(SOCK_NONBLOCK | SOCK_CLOEXEC)) return -EINVAL;
    struct process *process = process_current();
    if (domain == TUNIX_AF_UNIX) {
        if (base_type != TUNIX_SOCK_STREAM || protocol != 0) return -EOPNOTSUPP;
        struct unix_socket *socket = unix_socket_create();
        if (!socket) return -ENOMEM;
        unix_socket_set_credentials(socket, process ? (int32_t)process->pid : 0, 0, 0);
        struct file *file = file_create_socket(socket);
        if (!file) { unix_socket_unref(socket); return -ENOMEM; }
        file->flags = (uint32_t)(type_flags & SOCK_NONBLOCK);
        return install_new_file(file, type_flags & SOCK_CLOEXEC);
    }
    if (domain == TUNIX_AF_INET || domain == TUNIX_AF_PACKET) {
        struct inet_socket *socket = inet_socket_create(domain, base_type, protocol);
        if (!socket) return base_type == TUNIX_SOCK_STREAM ? -EOPNOTSUPP : -EPROTONOSUPPORT;
        struct file *file = file_create_inet_socket(socket);
        if (!file) { inet_socket_unref(socket); return -ENOMEM; }
        file->flags = (uint32_t)(type_flags & SOCK_NONBLOCK);
        return install_new_file(file, type_flags & SOCK_CLOEXEC);
    }
    if (domain == TUNIX_AF_NETLINK) {
        /* Netlink is message-oriented (SOCK_RAW/SOCK_DGRAM); the protocol
           argument selects the netlink family (NETLINK_ROUTE, SOCK_DIAG). */
        if (base_type != TUNIX_SOCK_RAW && base_type != TUNIX_SOCK_DGRAM) return -EPROTONOSUPPORT;
        struct netlink_socket *socket = netlink_socket_create(protocol);
        if (!socket) return -EPROTONOSUPPORT;
        struct file *file = file_create_netlink_socket(socket);
        if (!file) { netlink_socket_unref(socket); return -ENOMEM; }
        file->flags = (uint32_t)(type_flags & SOCK_NONBLOCK);
        return install_new_file(file, type_flags & SOCK_CLOEXEC);
    }
    return -EAFNOSUPPORT;
}

static int64_t sys_socketpair(int domain, int type, int protocol,
                              uint64_t user_fds) {
    int base_type = type & 0xF;
    int type_flags = type & ~0xF;
    if (type_flags & ~(SOCK_NONBLOCK | SOCK_CLOEXEC)) return -EINVAL;
    if (domain != TUNIX_AF_UNIX || base_type != TUNIX_SOCK_STREAM || protocol != 0)
        return -EOPNOTSUPP;
    struct unix_socket *first = NULL;
    struct unix_socket *second = NULL;
    int status = unix_socket_pair(&first, &second);
    if (status < 0) return status;
    struct process *process = process_current();
    int32_t pid = process ? (int32_t)process->pid : 0;
    unix_socket_set_credentials(first, pid, 0, 0);
    unix_socket_set_credentials(second, pid, 0, 0);
    struct file *first_file = file_create_socket(first);
    struct file *second_file = file_create_socket(second);
    if (!first_file || !second_file) {
        if (first_file) file_unref(first_file); else unix_socket_unref(first);
        if (second_file) file_unref(second_file); else unix_socket_unref(second);
        return -ENOMEM;
    }
    first_file->flags = (uint32_t)(type_flags & SOCK_NONBLOCK);
    second_file->flags = (uint32_t)(type_flags & SOCK_NONBLOCK);
    uint8_t fd_flags = (type_flags & SOCK_CLOEXEC) ? PROCESS_FD_CLOEXEC : 0;
    int first_fd = process_install_file_flags(process, first_file, 0, fd_flags);
    int second_fd = process_install_file_flags(process, second_file, 0, fd_flags);
    if (first_fd < 0 || second_fd < 0) {
        if (first_fd >= 0) process_close_fd(process, first_fd); else file_unref(first_file);
        if (second_fd >= 0) process_close_fd(process, second_fd); else file_unref(second_file);
        return -EMFILE;
    }
    int fds[2] = {first_fd, second_fd};
    if (copy_to_user(user_fds, fds, sizeof(fds)) != 0) {
        process_close_fd(process, first_fd);
        process_close_fd(process, second_fd);
        return -EFAULT;
    }
    return 0;
}

static int copy_sockaddr_un(uint64_t user_address, uint64_t length,
                            struct tunix_sockaddr_un *address) {
    if (!user_address || !address || length < sizeof(uint16_t)) return -EINVAL;
    if (length > sizeof(*address)) length = sizeof(*address);
    memset(address, 0, sizeof(*address));
    return copy_from_user(address, user_address, (size_t)length) == 0 ? 0 : -EFAULT;
}

static int64_t sys_bind(int fd, uint64_t user_address, uint64_t length) {
    struct unix_socket *unix_value = socket_from_fd(fd);
    if (unix_value) {
        struct tunix_sockaddr_un address;
        int status = copy_sockaddr_un(user_address, length, &address);
        return status < 0 ? status : unix_socket_bind(unix_value, &address, (size_t)length);
    }
    struct netlink_socket *netlink_value = netlink_socket_from_fd(fd);
    if (netlink_value) {
        uint8_t address[32];
        if (length > sizeof(address)) length = sizeof(address);
        if (length && copy_from_user(address, user_address, (size_t)length) != 0) return -EFAULT;
        return netlink_socket_bind(netlink_value, length ? address : NULL, (size_t)length);
    }
    struct inet_socket *inet_value = inet_socket_from_fd(fd);
    if (!inet_value || !user_address || length < 2 || length > 32) return -EBADF;
    uint8_t address[32];
    if (copy_from_user(address, user_address, (size_t)length) != 0) return -EFAULT;
    return inet_socket_bind(inet_value, address, (size_t)length);
}

static int64_t sys_listen(int fd, int backlog) {
    struct unix_socket *socket = socket_from_fd(fd);
    return socket ? unix_socket_listen(socket, backlog) : -EBADF;
}

static int64_t sys_connect(int fd, uint64_t user_address, uint64_t length) {
    struct unix_socket *unix_value = socket_from_fd(fd);
    if (unix_value) {
        struct tunix_sockaddr_un address;
        int status = copy_sockaddr_un(user_address, length, &address);
        return status < 0 ? status : unix_socket_connect(unix_value, &address, (size_t)length);
    }
    struct inet_socket *inet_value = inet_socket_from_fd(fd);
    if (!inet_value || !user_address || length < 2 || length > 32) return -EBADF;
    uint8_t address[32];
    if (copy_from_user(address, user_address, (size_t)length) != 0) return -EFAULT;
    return inet_socket_connect(inet_value, address, (size_t)length);
}

static int64_t sys_shutdown(int fd, int how) {
    struct process *process = process_current();
    if (!process || fd < 0 || fd >= PROCESS_MAX_FDS || !process->fds[fd]) return -EBADF;
    struct file *file = process->fds[fd];
    if (file->kind == FILE_KIND_SOCKET) return unix_socket_shutdown(file->socket, how);
    if (file->kind == FILE_KIND_INET_SOCKET) return inet_socket_shutdown(file->inet_socket, how);
    return -ENOTSOCK;
}

static int64_t sys_accept(int fd, uint64_t user_address, uint64_t user_length, int flags) {
    (void)user_address;
    (void)user_length;
    if (flags & ~(O_NONBLOCK | O_CLOEXEC)) return -EINVAL;
    struct unix_socket *listener = socket_from_fd(fd);
    if (!listener || !unix_socket_is_listener(listener)) return -EBADF;
    struct unix_socket *accepted = unix_socket_accept(listener);
    if (!accepted) return -EAGAIN;
    struct file *file = file_create_socket(accepted);
    if (!file) {
        unix_socket_unref(accepted);
        return -ENOMEM;
    }
    file->flags = (uint32_t)(flags & O_NONBLOCK);
    int new_fd = process_install_file_flags(process_current(), file, 0,
        (flags & O_CLOEXEC) ? PROCESS_FD_CLOEXEC : 0);
    if (new_fd < 0) {
        file_unref(file);
        return -EMFILE;
    }
    return new_fd;
}

static int64_t sys_sendto(int fd, uint64_t user_data, size_t length, int flags,
                          uint64_t user_address, uint64_t address_length) {
    struct netlink_socket *netlink = netlink_socket_from_fd(fd);
    if (netlink) {
        if (length > 4096U) return -EMSGSIZE;
        uint8_t request[4096];
        if (length && copy_from_user(request, user_data, length) != 0) return -EFAULT;
        return netlink_socket_sendto(netlink, request, length, flags, NULL, 0);
    }
    struct inet_socket *socket = inet_socket_from_fd(fd);
    if (!socket) return -EBADF;
    if (length > 2048U) return -EMSGSIZE;
    uint8_t data[2048];
    uint8_t address[32];
    if (length && copy_from_user(data, user_data, length) != 0) return -EFAULT;
    const void *address_pointer = NULL;
    if (user_address) {
        if (address_length < 2 || address_length > sizeof(address)) return -EINVAL;
        if (copy_from_user(address, user_address, (size_t)address_length) != 0) return -EFAULT;
        address_pointer = address;
    }
    return inet_socket_sendto(socket, data, length, flags, address_pointer, (size_t)address_length);
}

static int64_t sys_recvfrom(int fd, uint64_t user_data, size_t length, int flags,
                            uint64_t user_address, uint64_t user_address_length) {
    struct netlink_socket *netlink = netlink_socket_from_fd(fd);
    if (netlink) {
        if (length > 4096U) length = 4096U;
        uint8_t data[4096];
        struct tunix_sockaddr_nl nl_address;
        size_t nl_length = sizeof(nl_address);
        int64_t result = netlink_socket_recvfrom(netlink, data, length, flags,
                                                 user_address ? &nl_address : NULL,
                                                 user_address ? &nl_length : NULL);
        if (result < 0) return result;
        if (result && copy_to_user(user_data, data, (size_t)result) != 0) return -EFAULT;
        if (user_address) {
            if (copy_to_user(user_address, &nl_address, nl_length) != 0) return -EFAULT;
            if (user_address_length) {
                uint32_t output_length = (uint32_t)nl_length;
                if (copy_to_user(user_address_length, &output_length, sizeof(output_length)) != 0) return -EFAULT;
            }
        }
        return result;
    }
    struct inet_socket *socket = inet_socket_from_fd(fd);
    if (!socket) return -EBADF;
    if (length > 2048U) length = 2048U;
    uint8_t data[2048];
    uint8_t address[32];
    size_t address_length = sizeof(address);
    if (user_address_length) {
        uint32_t supplied;
        if (copy_from_user(&supplied, user_address_length, sizeof(supplied)) != 0) return -EFAULT;
        address_length = supplied < sizeof(address) ? supplied : sizeof(address);
    }
    int64_t result = inet_socket_recvfrom(socket, data, length, flags,
                                           user_address ? address : NULL,
                                           user_address ? &address_length : NULL);
    if (result < 0) return result;
    if (result && copy_to_user(user_data, data, (size_t)result) != 0) return -EFAULT;
    if (user_address) {
        if (copy_to_user(user_address, address, address_length) != 0) return -EFAULT;
        if (user_address_length) {
            uint32_t output_length = (uint32_t)address_length;
            if (copy_to_user(user_address_length, &output_length, sizeof(output_length)) != 0) return -EFAULT;
        }
    }
    return result;
}

static int copy_message_iovecs(const struct linux_msghdr *message, uint8_t *buffer,
                               size_t capacity, size_t *total, int from_user) {
    if (!message || !buffer || !total || message->iov_length > 16U) return -EINVAL;
    size_t completed = 0;
    for (uint64_t index = 0; index < message->iov_length; index++) {
        struct linux_iovec iov;
        if (copy_from_user(&iov, message->iov + index * sizeof(iov), sizeof(iov)) != 0)
            return -EFAULT;
        if (iov.length > capacity - completed) return -EMSGSIZE;
        if (iov.length) {
            int status = from_user
                ? copy_from_user(buffer + completed, iov.base, (size_t)iov.length)
                : copy_to_user(iov.base, buffer + completed, (size_t)iov.length);
            if (status != 0) return -EFAULT;
        }
        completed += (size_t)iov.length;
    }
    *total = completed;
    return 0;
}

static void release_file_array(struct file **files, size_t count) {
    for (size_t index = 0; index < count; index++) if (files[index]) file_unref(files[index]);
}

static int collect_scm_rights(const struct linux_msghdr *message,
                              struct file **files, size_t *file_count) {
    *file_count = 0;
    if (!message->control || !message->control_length) return 0;
    size_t offset = 0;
    while (offset + sizeof(struct linux_cmsghdr) <= message->control_length) {
        struct linux_cmsghdr header;
        if (copy_from_user(&header, message->control + offset, sizeof(header)) != 0) {
            release_file_array(files, *file_count);
            return -EFAULT;
        }
        if (header.length < sizeof(header) ||
            header.length > message->control_length - offset) {
            release_file_array(files, *file_count);
            return -EINVAL;
        }
        size_t payload = (size_t)header.length - sizeof(header);
        if (header.level == SOL_SOCKET && header.type == SCM_RIGHTS) {
            if (payload % sizeof(int32_t)) {
                release_file_array(files, *file_count);
                return -EINVAL;
            }
            size_t amount = payload / sizeof(int32_t);
            if (amount > 8U - *file_count) {
                release_file_array(files, *file_count);
                return -EMSGSIZE;
            }
            for (size_t index = 0; index < amount; index++) {
                int32_t descriptor;
                uint64_t user_fd = message->control + offset + sizeof(header) +
                                   index * sizeof(descriptor);
                if (copy_from_user(&descriptor, user_fd, sizeof(descriptor)) != 0) {
                    release_file_array(files, *file_count);
                    return -EFAULT;
                }
                struct file *file = file_from_fd(descriptor);
                if (!file) {
                    release_file_array(files, *file_count);
                    return -EBADF;
                }
                file_ref(file);
                files[(*file_count)++] = file;
            }
        }
        size_t step = cmsg_align((size_t)header.length);
        if (!step || step > message->control_length - offset) break;
        offset += step;
    }
    return 0;
}

static int64_t sys_sendmsg(int fd, uint64_t user_message, int flags) {
    if (!user_message) return -EFAULT;
    struct linux_msghdr message;
    if (copy_from_user(&message, user_message, sizeof(message)) != 0) return -EFAULT;
    uint8_t data[4096];
    size_t length = 0;
    int status = copy_message_iovecs(&message, data, sizeof(data), &length, 1);
    if (status < 0) return status;

    struct unix_socket *unix_value = socket_from_fd(fd);
    if (unix_value) {
        if (message.name) return -EISDIR;
        struct file *files[8] = {0};
        size_t file_count = 0;
        status = collect_scm_rights(&message, files, &file_count);
        if (status < 0) return status;
        if (file_count && length == 0) {
            release_file_array(files, file_count);
            return -EINVAL;
        }
        int64_t result = unix_socket_send_with_rights(unix_value, length, data,
                                                       files, file_count);
        if (result < 0) release_file_array(files, file_count);
        return result;
    }

    struct netlink_socket *netlink = netlink_socket_from_fd(fd);
    if (netlink)
        return netlink_socket_sendto(netlink, data, length, flags, NULL, 0);

    struct inet_socket *socket = inet_socket_from_fd(fd);
    if (!socket) return -EBADF;
    uint8_t address[32];
    const void *address_pointer = NULL;
    if (message.name) {
        if (message.name_length < 2U || message.name_length > sizeof(address)) return -EINVAL;
        if (copy_from_user(address, message.name, message.name_length) != 0) return -EFAULT;
        address_pointer = address;
    }
    return inet_socket_sendto(socket, data, length, flags, address_pointer,
                              message.name ? message.name_length : 0U);
}

static int scatter_message_data(const struct linux_msghdr *message,
                                const uint8_t *data, size_t length) {
    size_t remaining = length;
    size_t offset = 0;
    for (uint64_t index = 0; index < message->iov_length && remaining; index++) {
        struct linux_iovec iov;
        if (copy_from_user(&iov, message->iov + index * sizeof(iov), sizeof(iov)) != 0)
            return -EFAULT;
        size_t amount = iov.length < remaining ? (size_t)iov.length : remaining;
        if (amount && copy_to_user(iov.base, data + offset, amount) != 0) return -EFAULT;
        offset += amount;
        remaining -= amount;
    }
    return 0;
}

static int write_unix_control(struct linux_msghdr *message,
                              struct unix_socket *socket,
                              struct file **files, size_t file_count,
                              int receive_flags) {
    struct unix_credentials peer = {0, 0, 0};
    int include_credentials = unix_socket_get_passcred(socket) &&
        unix_socket_get_peer_credentials(socket, &peer) == 0;
    size_t rights_length = file_count ?
        sizeof(struct linux_cmsghdr) + file_count * sizeof(int32_t) : 0;
    size_t rights_space = file_count ? cmsg_align(rights_length) : 0;
    size_t credentials_length = include_credentials ?
        sizeof(struct linux_cmsghdr) + sizeof(struct linux_ucred) : 0;
    size_t credentials_space = include_credentials ? cmsg_align(credentials_length) : 0;
    size_t required = rights_space + credentials_space;

    if (!required) {
        message->control_length = 0;
        return 0;
    }
    if (!message->control || message->control_length < required) {
        message->flags |= MSG_CTRUNC;
        message->control_length = 0;
        release_file_array(files, file_count);
        return 0;
    }

    int installed[8];
    size_t installed_count = 0;
    for (size_t index = 0; index < file_count; index++) {
        int descriptor = process_install_file_flags(process_current(), files[index], 0,
            (receive_flags & MSG_CMSG_CLOEXEC) ? PROCESS_FD_CLOEXEC : 0);
        if (descriptor < 0) {
            for (size_t rollback = 0; rollback < installed_count; rollback++)
                process_close_fd(process_current(), installed[rollback]);
            for (size_t remaining = index; remaining < file_count; remaining++)
                file_unref(files[remaining]);
            message->flags |= MSG_CTRUNC;
            message->control_length = 0;
            return 0;
        }
        installed[installed_count++] = descriptor;
    }

    size_t offset = 0;
    if (file_count) {
        struct linux_cmsghdr header = {rights_length, SOL_SOCKET, SCM_RIGHTS};
        if (copy_to_user(message->control + offset, &header, sizeof(header)) != 0 ||
            copy_to_user(message->control + offset + sizeof(header), installed,
                         installed_count * sizeof(installed[0])) != 0) {
            for (size_t index = 0; index < installed_count; index++)
                process_close_fd(process_current(), installed[index]);
            return -EFAULT;
        }
        if (rights_space > rights_length) {
            uint64_t zero = 0;
            if (copy_to_user(message->control + offset + rights_length, &zero,
                             rights_space - rights_length) != 0) {
                for (size_t index = 0; index < installed_count; index++)
                    process_close_fd(process_current(), installed[index]);
                return -EFAULT;
            }
        }
        offset += rights_space;
    }
    if (include_credentials) {
        struct linux_cmsghdr header = {credentials_length, SOL_SOCKET,
                                       SCM_CREDENTIALS};
        struct linux_ucred credentials = {peer.pid, peer.uid, peer.gid};
        if (copy_to_user(message->control + offset, &header, sizeof(header)) != 0 ||
            copy_to_user(message->control + offset + sizeof(header), &credentials,
                         sizeof(credentials)) != 0) {
            for (size_t index = 0; index < installed_count; index++)
                process_close_fd(process_current(), installed[index]);
            return -EFAULT;
        }
        if (credentials_space > credentials_length) {
            uint64_t zero = 0;
            if (copy_to_user(message->control + offset + credentials_length, &zero,
                             credentials_space - credentials_length) != 0) {
                for (size_t index = 0; index < installed_count; index++)
                    process_close_fd(process_current(), installed[index]);
                return -EFAULT;
            }
        }
        offset += credentials_space;
    }
    message->control_length = offset;
    return 0;
}

static int64_t sys_recvmsg(int fd, uint64_t user_message, int flags) {
    if (!user_message) return -EFAULT;
    struct linux_msghdr message;
    if (copy_from_user(&message, user_message, sizeof(message)) != 0) return -EFAULT;
    if (message.iov_length > 16U) return -EINVAL;

    /* Bounce buffer for a single datagram/stream read, kept small to stay well
       within the 16 KiB kernel stack. A caller may legitimately offer more room
       than we stage in one call -- musl's getaddrinfo() path (lookup_name.c
       ABUF_SIZE) hands us a 4800-byte iovec even though the DNS reply is tiny --
       so clamp the accepted capacity to the buffer instead of rejecting the
       request with -EMSGSIZE; a datagram larger than this is simply truncated. */
    uint8_t data[4096];

    size_t capacity = 0;
    for (uint64_t index = 0; index < message.iov_length; index++) {
        struct linux_iovec iov;
        if (copy_from_user(&iov, message.iov + index * sizeof(iov), sizeof(iov)) != 0)
            return -EFAULT;
        if (iov.length >= sizeof(data) - capacity) {
            capacity = sizeof(data);
            break;
        }
        capacity += (size_t)iov.length;
    }
    struct unix_socket *unix_value = socket_from_fd(fd);
    if (unix_value) {
        struct file *files[8] = {0};
        size_t file_count = 0;
        int64_t result = unix_socket_recv_with_rights(unix_value, capacity, data,
                                                       files, 8, &file_count);
        if (result < 0) return result;
        if (scatter_message_data(&message, data, (size_t)result) != 0) {
            release_file_array(files, file_count);
            return -EFAULT;
        }
        message.name_length = 0;
        message.flags = 0;
        int status = write_unix_control(&message, unix_value, files, file_count, flags);
        if (status < 0) return status;
        if (copy_to_user(user_message, &message, sizeof(message)) != 0) return -EFAULT;
        return result;
    }

    struct netlink_socket *netlink = netlink_socket_from_fd(fd);
    if (netlink) {
        struct tunix_sockaddr_nl nl_address;
        size_t nl_length = sizeof(nl_address);
        int64_t result = netlink_socket_recvfrom(netlink, data, capacity, flags,
                                                 message.name ? &nl_address : NULL,
                                                 message.name ? &nl_length : NULL);
        if (result < 0) return result;
        if (scatter_message_data(&message, data, (size_t)result) != 0) return -EFAULT;
        if (message.name) {
            size_t copy = message.name_length < nl_length ? message.name_length : nl_length;
            if (copy && copy_to_user(message.name, &nl_address, copy) != 0) return -EFAULT;
            message.name_length = (uint32_t)nl_length;
        }
        message.control_length = 0;
        message.flags = 0;
        if (copy_to_user(user_message, &message, sizeof(message)) != 0) return -EFAULT;
        return result;
    }

    struct inet_socket *socket = inet_socket_from_fd(fd);
    if (!socket) return -EBADF;
    uint8_t address[32];
    size_t address_length = message.name_length < sizeof(address)
        ? message.name_length : sizeof(address);
    int64_t result = inet_socket_recvfrom(socket, data, capacity, flags,
                                           message.name ? address : NULL,
                                           message.name ? &address_length : NULL);
    if (result < 0) return result;
    if (scatter_message_data(&message, data, (size_t)result) != 0) return -EFAULT;
    if (message.name) {
        if (copy_to_user(message.name, address, address_length) != 0) return -EFAULT;
        message.name_length = (uint32_t)address_length;
    }
    message.control_length = 0;
    message.flags = 0;
    if (copy_to_user(user_message, &message, sizeof(message)) != 0) return -EFAULT;
    return result;
}

static int64_t sys_socket_name(int fd, uint64_t user_address, uint64_t user_length, int peer) {
    if (!user_address || !user_length) return -EFAULT;
    uint32_t supplied;
    if (copy_from_user(&supplied, user_length, sizeof(supplied)) != 0) return -EFAULT;

    struct unix_socket *unix_value = socket_from_fd(fd);
    if (unix_value) {
        struct tunix_sockaddr_un address;
        size_t actual_length = 0;
        int status = unix_socket_get_name(unix_value, peer, &address, &actual_length);
        if (status < 0) return status;
        size_t copy_length = supplied < actual_length ? supplied : actual_length;
        if (copy_length && copy_to_user(user_address, &address, copy_length) != 0)
            return -EFAULT;
        uint32_t output_length = (uint32_t)actual_length;
        return copy_to_user(user_length, &output_length, sizeof(output_length)) == 0 ?
            0 : -EFAULT;
    }

    struct netlink_socket *netlink = netlink_socket_from_fd(fd);
    if (netlink) {
        if (peer) return -EOPNOTSUPP;
        struct tunix_sockaddr_nl nl;
        size_t length = sizeof(nl);
        int status = netlink_socket_getsockname(netlink, &nl, &length);
        if (status < 0) return status;
        size_t copy = supplied < length ? supplied : length;
        if (copy && copy_to_user(user_address, &nl, copy) != 0) return -EFAULT;
        uint32_t output_length = (uint32_t)length;
        return copy_to_user(user_length, &output_length, sizeof(output_length)) == 0 ? 0 : -EFAULT;
    }
    struct inet_socket *socket = inet_socket_from_fd(fd);
    if (!socket) return -EBADF;
    uint8_t address[32];
    size_t length = supplied < sizeof(address) ? supplied : sizeof(address);
    int status = peer ? inet_socket_getpeername(socket, address, &length)
                      : inet_socket_getsockname(socket, address, &length);
    if (status < 0) return status;
    if (copy_to_user(user_address, address, length) != 0) return -EFAULT;
    uint32_t output_length = (uint32_t)length;
    return copy_to_user(user_length, &output_length, sizeof(output_length)) == 0 ? 0 : -EFAULT;
}

static int64_t sys_setsockopt(int fd, int level, int option,
                                  uint64_t user_value, size_t length) {
    struct unix_socket *unix_value = socket_from_fd(fd);
    if (unix_value) {
        if (level != SOL_SOCKET || option != SO_PASSCRED || length < sizeof(int32_t))
            return -EOPNOTSUPP;
        int32_t enabled;
        if (copy_from_user(&enabled, user_value, sizeof(enabled)) != 0) return -EFAULT;
        unix_socket_set_passcred(unix_value, enabled != 0);
        return 0;
    }
    if (netlink_socket_from_fd(fd)) {
        /* iproute2 sets SO_SNDBUF/SO_RCVBUF and a few SOL_NETLINK options while
           opening the socket; accept them so rtnl_open() does not bail out. */
        (void)level; (void)option; (void)user_value; (void)length;
        return 0;
    }
    struct inet_socket *socket = inet_socket_from_fd(fd);
    if (!socket) return -EBADF;
    if (length > 256U) return -EINVAL;
    uint8_t value[256];
    if (length && copy_from_user(value, user_value, length) != 0) return -EFAULT;
    return inet_socket_setsockopt(socket, level, option, value, length);
}

static int64_t sys_getsockopt(int fd, int level, int option,
                              uint64_t user_value, uint64_t user_length) {
    if (!user_length) return -EFAULT;
    uint32_t supplied;
    if (copy_from_user(&supplied, user_length, sizeof(supplied)) != 0) return -EFAULT;
    struct unix_socket *unix_value = socket_from_fd(fd);
    if (unix_value) {
        if (level != SOL_SOCKET) return -EOPNOTSUPP;
        if (option == SO_TYPE || option == SO_ERROR || option == SO_ACCEPTCONN) {
            if (supplied < sizeof(int32_t)) return -EINVAL;
            int32_t value = option == SO_TYPE ? TUNIX_SOCK_STREAM :
                (option == SO_ACCEPTCONN ? unix_socket_is_listener(unix_value) : 0);
            if (copy_to_user(user_value, &value, sizeof(value)) != 0) return -EFAULT;
            uint32_t length = sizeof(value);
            return copy_to_user(user_length, &length, sizeof(length)) == 0 ? 0 : -EFAULT;
        }
        if (option == SO_PEERCRED) {
            if (supplied < sizeof(struct linux_ucred)) return -EINVAL;
            struct unix_credentials peer;
            int status = unix_socket_get_peer_credentials(unix_value, &peer);
            if (status < 0) return status;
            struct linux_ucred value = {peer.pid, peer.uid, peer.gid};
            if (copy_to_user(user_value, &value, sizeof(value)) != 0) return -EFAULT;
            uint32_t length = sizeof(value);
            return copy_to_user(user_length, &length, sizeof(length)) == 0 ? 0 : -EFAULT;
        }
        if (option == SO_PASSCRED) {
            if (supplied < sizeof(int32_t)) return -EINVAL;
            int32_t enabled = unix_socket_get_passcred(unix_value);
            if (copy_to_user(user_value, &enabled, sizeof(enabled)) != 0) return -EFAULT;
            uint32_t length = sizeof(enabled);
            return copy_to_user(user_length, &length, sizeof(length)) == 0 ? 0 : -EFAULT;
        }
        return -EOPNOTSUPP;
    }
    if (netlink_socket_from_fd(fd)) {
        /* Report a plausible buffer size for any query so iproute2's socket
           setup path never trips on an error. */
        (void)level; (void)option;
        int32_t value = 32768;
        if (supplied < sizeof(value)) return -EINVAL;
        if (copy_to_user(user_value, &value, sizeof(value)) != 0) return -EFAULT;
        uint32_t output_length = sizeof(value);
        return copy_to_user(user_length, &output_length, sizeof(output_length)) == 0 ? 0 : -EFAULT;
    }
    struct inet_socket *socket = inet_socket_from_fd(fd);
    if (!socket) return -EBADF;
    uint8_t value[256];
    size_t length = supplied < sizeof(value) ? supplied : sizeof(value);
    int status = inet_socket_getsockopt(socket, level, option, value, &length);
    if (status < 0) return status;
    if (copy_to_user(user_value, value, length) != 0) return -EFAULT;
    uint32_t output_length = (uint32_t)length;
    return copy_to_user(user_length, &output_length, sizeof(output_length)) == 0 ? 0 : -EFAULT;
}

static int64_t sys_ftruncate(int fd, uint64_t length) {
    struct process *process = process_current();
    if (!process || fd < 0 || fd >= PROCESS_MAX_FDS || !process->fds[fd]) return -EBADF;
    struct file *file = process->fds[fd];
    if (file->kind != FILE_KIND_VFS || !file->node) return -EINVAL;
    return vfs_truncate(file->node, length) == 0 ? 0 : -EIO;
}

static int64_t sys_fsync(int fd) {
    struct process *process = process_current();
    if (!process || fd < 0 || fd >= PROCESS_MAX_FDS || !process->fds[fd]) return -EBADF;
    struct file *file = process->fds[fd];
    if (file->kind == FILE_KIND_FRAMEBUFFER) return 0;
    if (file->kind != FILE_KIND_VFS || !file->node) return -EINVAL;
    uint32_t node_type = file->node->flags & 0xFFU;
    if (node_type == VFS_FILE || node_type == VFS_DIRECTORY || node_type == VFS_BLOCKDEVICE) {
        if (ext2fs_owns(file->node) && ext2fs_fsync_node(file->node) != 0) return -EIO;
        return 0;
    }
    return -EINVAL;
}

static int64_t sys_ioctl(int fd, unsigned long request, uint64_t user_argument) {
    struct process *process = process_current();
    if (!process || fd < 0 || fd >= PROCESS_MAX_FDS || !process->fds[fd]) return -EBADF;
    struct file *file = process->fds[fd];
    if (file->kind == FILE_KIND_INET_SOCKET && request == SIOCGIFCONF) {
        if (!user_argument) return -EFAULT;
        struct linux_ifconf ifconf;
        if (copy_from_user(&ifconf, user_argument, sizeof(ifconf)) != 0) return -EFAULT;
        if (ifconf.length < 0) return -EINVAL;

        struct linux_ifreq ifreq;
        memset(&ifreq, 0, sizeof(ifreq));
        memcpy(ifreq.name, "eth0", 5);
        ifreq.value[0] = 2; /* AF_INET in sockaddr.sa_family */
        const struct net_config *config = net_get_config();
        memcpy(ifreq.value + 4, &config->address, sizeof(config->address));

        if (!ifconf.buffer) {
            ifconf.length = (int32_t)sizeof(ifreq);
        } else if ((size_t)ifconf.length >= sizeof(ifreq)) {
            if (copy_to_user(ifconf.buffer, &ifreq, sizeof(ifreq)) != 0) return -EFAULT;
            ifconf.length = (int32_t)sizeof(ifreq);
        } else {
            ifconf.length = 0;
        }
        return copy_to_user(user_argument, &ifconf, sizeof(ifconf)) == 0 ? 0 : -EFAULT;
    }
    if (file->kind == FILE_KIND_PTY_MASTER || file->kind == FILE_KIND_PTY_SLAVE)
        return pty_ioctl(file->pty, file->kind == FILE_KIND_PTY_MASTER,
                         request, user_argument);
    if (file->kind == FILE_KIND_INPUT && file->node && file->node->ioctl)
        return file->node->ioctl(file->node, request, user_argument);
    if (file->kind == FILE_KIND_FRAMEBUFFER)
        return framebuffer_file_ioctl(file, request, user_argument);
    if (file->kind == FILE_KIND_INET_SOCKET) {
        size_t argument_size = (request == 0x890BU || request == 0x890CU) ? 128U : 40U;
        uint8_t argument[128];
        if (!user_argument || copy_from_user(argument, user_argument, argument_size) != 0) return -EFAULT;
        int status = inet_socket_ioctl(file->inet_socket, request, argument);
        if (status < 0) return status;
        return copy_to_user(user_argument, argument, argument_size) == 0 ? 0 : -EFAULT;
    }
    if (file->kind != FILE_KIND_VFS || !file->node || (file->node->flags & 0xFFU) != VFS_CHARDEVICE) return -ENOTTY;
    if (file->node->ioctl) return file->node->ioctl(file->node, request, user_argument);

    if (request == TCGETS) {
        struct tunix_termios value;
        if (tty_ioctl(request, &value) != 0) return -ENOTTY;
        return copy_to_user(user_argument, &value, sizeof(value)) == 0 ? 0 : -EFAULT;
    }
    if (request == TCSETS || request == TCSETSW || request == TCSETSF) {
        struct tunix_termios value;
        if (copy_from_user(&value, user_argument, sizeof(value)) != 0) return -EFAULT;
        return tty_ioctl(request, &value) == 0 ? 0 : -ENOTTY;
    }
    if (request == TIOCGPGRP) {
        int pgid;
        if (tty_ioctl(request, &pgid) != 0) return -ENOTTY;
        return copy_to_user(user_argument, &pgid, sizeof(pgid)) == 0 ? 0 : -EFAULT;
    }
    if (request == TIOCSPGRP) {
        int pgid;
        if (copy_from_user(&pgid, user_argument, sizeof(pgid)) != 0) return -EFAULT;
        return tty_ioctl(request, &pgid) == 0 ? 0 : -ENOTTY;
    }
    if (request == TIOCGETD || request == TIOCSETD) {
        int discipline = 0;
        if (request == TIOCSETD && copy_from_user(&discipline, user_argument, sizeof(discipline)) != 0) return -EFAULT;
        if (tty_ioctl(request, &discipline) != 0) return -ENOTTY;
        if (request == TIOCGETD && copy_to_user(user_argument, &discipline, sizeof(discipline)) != 0) return -EFAULT;
        return 0;
    }
    if (request == TUNIX_KDGKBMAP) {
        struct tunix_keymap map;
        if (tty_ioctl(request, &map) != 0) return -ENOTTY;
        return copy_to_user(user_argument, &map, sizeof(map)) == 0 ? 0 : -EFAULT;
    }
    if (request == TUNIX_KDSKBMAP) {
        struct tunix_keymap map;
        if (copy_from_user(&map, user_argument, sizeof(map)) != 0) return -EFAULT;
        return tty_ioctl(request, &map) == 0 ? 0 : -EINVAL;
    }
    if (request == TIOCGWINSZ) {
        uint16_t rows;
        uint16_t columns;
        terminal_get_dimensions(&rows, &columns);
        struct linux_winsize winsize = {rows, columns, 0, 0};
        return copy_to_user(user_argument, &winsize, sizeof(winsize)) == 0 ? 0 : -EFAULT;
    }
    return -ENOTTY;
}

static void fill_stat(struct vfs_node *node, struct linux_stat *stat) {
    memset(stat, 0, sizeof(*stat));
    stat->st_ino = node->inode;
    stat->st_nlink = 1;
    stat->st_uid = node->uid;
    stat->st_gid = node->gid;
    stat->st_size = (int64_t)node->length;
    stat->st_blksize = 4096;
    stat->st_blocks = (int64_t)((node->length + 511) / 512);
    uint32_t kind = node->flags & 0xFFU;
    uint32_t type = kind == VFS_DIRECTORY ? 0040000U :
                    (kind == VFS_CHARDEVICE ? 0020000U :
                    (kind == VFS_BLOCKDEVICE ? 0060000U :
                    (kind == VFS_SYMLINK ? 0120000U : 0100000U)));
    stat->st_mode = type | (node->mode & 0777U);
    /* ext2 rev 0 has no sub-second field, so the nanosecond parts stay zero. */
    stat->st_atim.tv_sec = (int64_t)node->atime;
    stat->st_mtim.tv_sec = (int64_t)node->mtime;
    stat->st_ctim.tv_sec = (int64_t)node->ctime;
}

static int64_t stat_path(int dirfd, uint64_t user_path, uint64_t user_stat, int follow) {
    char path[256];
    int status = copy_path_at(dirfd, user_path, path);
    if (status != 0) return status;
    struct vfs_node *node = follow ? vfs_lookup(path) : vfs_lookup_nofollow(path);
    if (!node) return -ENOENT;
    struct linux_stat stat;
    fill_stat(node, &stat);
    return copy_to_user(user_stat, &stat, sizeof(stat)) == 0 ? 0 : -EFAULT;
}

/*
 * Tunix has one VFS tree rather than real mounts, so the filesystem a node
 * belongs to is derived from the tree: the volatile top-level directories
 * (/tmp, /run, /var/tmp, /dev, /proc) are RAM-only, everything else lives on
 * the ext2 root. Walk up to the nearest volatile ancestor to classify.
 */
static void fill_statfs(struct vfs_node *node, struct linux_statfs *out) {
    memset(out, 0, sizeof(*out));
    /* The VFS name field caps a component at 127 characters plus the NUL. */
    out->f_namelen = sizeof(node->name) - 1;

    struct vfs_node *volatile_root = NULL;
    for (struct vfs_node *walk = node; walk; walk = walk->parent) {
        if (walk->flags & VFS_VOLATILE) volatile_root = walk;
        /* vfs_root is its own parent (vfs_init), so stop rather than spin. */
        if (walk->parent == walk) break;
    }

    if (volatile_root) {
        /* No per-filesystem quota exists, so report the RAM these share. */
        out->f_type = strcmp(volatile_root->name, "proc") == 0
                          ? PROC_SUPER_MAGIC : TMPFS_MAGIC;
        out->f_bsize = PMM_PAGE_SIZE;
        out->f_frsize = PMM_PAGE_SIZE;
        if (out->f_type == TMPFS_MAGIC) {
            out->f_blocks = pmm_total_page_count();
            out->f_bfree = pmm_free_page_count();
            out->f_bavail = out->f_bfree;
        }
        return;
    }

    struct ext2_fs_stats stats;
    if (ext2fs_stats(&stats) == 0) {
        out->f_type = EXT2_SUPER_MAGIC;
        out->f_bsize = stats.block_size;
        out->f_frsize = stats.block_size;
        out->f_blocks = stats.blocks;
        out->f_bfree = stats.free_blocks;
        out->f_bavail = stats.free_blocks > stats.reserved_blocks
                            ? stats.free_blocks - stats.reserved_blocks : 0;
        out->f_files = stats.inodes;
        out->f_ffree = stats.free_inodes;
        return;
    }

    /* Running from the initramfs, before any ext2 root is mounted. */
    out->f_type = TMPFS_MAGIC;
    out->f_bsize = PMM_PAGE_SIZE;
    out->f_frsize = PMM_PAGE_SIZE;
    out->f_blocks = pmm_total_page_count();
    out->f_bfree = pmm_free_page_count();
    out->f_bavail = out->f_bfree;
}

static int64_t sys_statfs(uint64_t user_path, uint64_t user_buf) {
    char path[256];
    int status = copy_path_at(AT_FDCWD, user_path, path);
    if (status != 0) return status;
    struct vfs_node *node = vfs_lookup(path);
    if (!node) return -ENOENT;
    struct linux_statfs out;
    fill_statfs(node, &out);
    return copy_to_user(user_buf, &out, sizeof(out)) == 0 ? 0 : -EFAULT;
}

static int64_t sys_fstatfs(int fd, uint64_t user_buf) {
    struct process *process = process_current();
    if (!process || fd < 0 || fd >= PROCESS_MAX_FDS || !process->fds[fd]) return -EBADF;
    struct file *file = process->fds[fd];
    if (file->kind != FILE_KIND_VFS || !file->node) return -EBADF;
    struct linux_statfs out;
    fill_statfs(file->node, &out);
    return copy_to_user(user_buf, &out, sizeof(out)) == 0 ? 0 : -EFAULT;
}

/*
 * Pipes and sockets have no VFS node, but fstat still has to describe them.
 * Reporting EBADF for a perfectly good descriptor is what made `cat file | wc`
 * fail: cat fstats its stdout to size its copy buffer, and grep fstats its
 * stdin. Linux answers with S_IFIFO / S_IFSOCK, so do the same.
 */
static int fill_stat_nodeless(struct file *file, struct linux_stat *stat) {
    uint32_t type;
    switch (file->kind) {
        case FILE_KIND_PIPE_READ:
        case FILE_KIND_PIPE_WRITE:
            type = 0010000U; /* S_IFIFO */
            break;
        case FILE_KIND_SOCKET:
        case FILE_KIND_INET_SOCKET:
        case FILE_KIND_NETLINK_SOCKET:
            type = 0140000U; /* S_IFSOCK */
            break;
        default:
            return -1;
    }
    memset(stat, 0, sizeof(*stat));
    stat->st_mode = type | 0600U;
    stat->st_nlink = 1;
    stat->st_blksize = 4096;
    /* No inode namespace for these, so give each open a stable unique value. */
    stat->st_ino = (uint64_t)(uintptr_t)file;
    return 0;
}

static int stat_from_file(struct file *file, struct linux_stat *stat) {
    if (!file) return -1;
    if (file->node &&
        (file->kind == FILE_KIND_VFS || file->kind == FILE_KIND_PTY_MASTER ||
         file->kind == FILE_KIND_PTY_SLAVE || file->kind == FILE_KIND_INPUT ||
         file->kind == FILE_KIND_FRAMEBUFFER)) {
        fill_stat(file->node, stat);
        return 0;
    }
    return fill_stat_nodeless(file, stat);
}

static int64_t sys_fstat(int fd, uint64_t user_stat) {
    struct process *process = process_current();
    if (!process || fd < 0 || fd >= PROCESS_MAX_FDS || !process->fds[fd]) return -EBADF;
    struct linux_stat stat;
    if (stat_from_file(process->fds[fd], &stat) != 0) return -EBADF;
    return copy_to_user(user_stat, &stat, sizeof(stat)) == 0 ? 0 : -EFAULT;
}

/*
 * STATX_BTIME is deliberately left out of the reported mask rather than
 * answered with a zero: callers test the mask before trusting a birth time,
 * and ext2 rev 0 has no field to store one in.
 */
static void fill_statx(const struct linux_stat *basic_in, struct linux_statx *out) {
    struct linux_stat basic = *basic_in;

    memset(out, 0, sizeof(*out));
    out->stx_mask = STATX_BASIC_STATS;
    out->stx_blksize = (uint32_t)basic.st_blksize;
    out->stx_nlink = (uint32_t)basic.st_nlink;
    out->stx_uid = basic.st_uid;
    out->stx_gid = basic.st_gid;
    out->stx_mode = (uint16_t)basic.st_mode;
    out->stx_ino = basic.st_ino;
    out->stx_size = (uint64_t)basic.st_size;
    out->stx_blocks = (uint64_t)basic.st_blocks;
    out->stx_atime.tv_sec = basic.st_atim.tv_sec;
    out->stx_mtime.tv_sec = basic.st_mtim.tv_sec;
    out->stx_ctime.tv_sec = basic.st_ctim.tv_sec;
}

static int64_t sys_statx(int dirfd, uint64_t user_path, int flags,
                         uint32_t mask, uint64_t user_buf) {
    (void)mask; /* Everything we can answer is cheap, so nothing is skipped. */
    if (flags & ~(AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH | AT_NO_AUTOMOUNT |
                  AT_STATX_SYNC_TYPE))
        return -EINVAL;

    char first = 0;
    if (copy_from_user(&first, user_path, 1) != 0) return -EFAULT;

    struct linux_stat basic;
    if (!first && (flags & AT_EMPTY_PATH) && dirfd >= 0) {
        struct process *process = process_current();
        if (!process || dirfd >= PROCESS_MAX_FDS || !process->fds[dirfd]) return -EBADF;
        if (stat_from_file(process->fds[dirfd], &basic) != 0) return -EBADF;
    } else {
        char path[256];
        int status = copy_path_at(dirfd, user_path, path);
        if (status != 0) return status;
        struct vfs_node *node = (flags & AT_SYMLINK_NOFOLLOW) ? vfs_lookup_nofollow(path)
                                                              : vfs_lookup(path);
        if (!node) return -ENOENT;
        fill_stat(node, &basic);
    }

    struct linux_statx out;
    fill_statx(&basic, &out);
    return copy_to_user(user_buf, &out, sizeof(out)) == 0 ? 0 : -EFAULT;
}

static int64_t sys_lseek(int fd, int64_t offset, int whence) {
    struct process *process = process_current();
    if (!process || fd < 0 || fd >= PROCESS_MAX_FDS || !process->fds[fd]) return -EBADF;
    struct file *file = process->fds[fd];
    if ((file->kind != FILE_KIND_VFS && file->kind != FILE_KIND_FRAMEBUFFER) ||
        !file->node) return -ESPIPE;
    int64_t base;
    if (whence == SEEK_SET) base = 0;
    else if (whence == SEEK_CUR) base = (int64_t)file->offset;
    else if (whence == SEEK_END) base = (int64_t)file->node->length;
    else return -EINVAL;
    if ((offset < 0 && base < -offset) || (offset > 0 && base > INT64_MAX - offset)) return -EINVAL;
    int64_t result = base + offset;
    if (result < 0) return -EINVAL;
    file->offset = (uint64_t)result;
    return result;
}

static int64_t sys_getdents64(int fd, uint64_t user_buffer, size_t length) {
    struct process *process = process_current();
    if (!process || fd < 0 || fd >= PROCESS_MAX_FDS || !process->fds[fd]) return -EBADF;
    struct file *file = process->fds[fd];
    if (file->kind != FILE_KIND_VFS || !file->node || (file->node->flags & 0xFFU) != VFS_DIRECTORY) return -ENOTDIR;
    size_t written = 0;
    while (written + 24 <= length) {
        struct dirent entry;
        int result = vfs_readdir(file->node, file->offset, &entry);
        if (result < 0) return -EIO;
        if (result == 0) break;
        size_t name_length = strlen(entry.name) + 1;
        size_t record_length = (19 + name_length + 7) & ~7ULL;
        if (written + record_length > length) break;
        uint8_t record[256];
        if (record_length > sizeof(record)) return -EIO;
        memset(record, 0, record_length);
        *(uint64_t *)(void *)(record + 0) = entry.ino;
        *(int64_t *)(void *)(record + 8) = (int64_t)(file->offset + 1);
        *(uint16_t *)(void *)(record + 16) = (uint16_t)record_length;
        record[18] = entry.type == VFS_DIRECTORY ? 4 :
                     (entry.type == VFS_CHARDEVICE ? 2 :
                     (entry.type == VFS_SYMLINK ? 10 : 8));
        memcpy(record + 19, entry.name, name_length);
        if (copy_to_user(user_buffer + written, record, record_length) != 0) return written ? (int64_t)written : -EFAULT;
        written += record_length;
        file->offset++;
    }
    return (int64_t)written;
}

static int64_t sys_getcwd(uint64_t user_buffer, size_t size) {
    struct process *process = process_current();
    if (!process || !user_buffer || size == 0) return -EINVAL;
    char path[256];
    if (vfs_node_path(process->cwd, path, sizeof(path)) != 0) return -EINVAL;
    size_t length = strlen(path) + 1;
    if (length > size) return -ERANGE;
    return copy_to_user(user_buffer, path, length) == 0 ? (int64_t)user_buffer : -EFAULT;
}

static int64_t sys_chdir(uint64_t user_path) {
    char path[256];
    int status = copy_path_at(AT_FDCWD, user_path, path);
    if (status != 0) return status;
    struct vfs_node *node = vfs_lookup(path);
    if (!node) return -ENOENT;
    if ((node->flags & 0xFFU) != VFS_DIRECTORY) return -ENOTDIR;
    process_current()->cwd = node;
    return 0;
}

static int64_t sys_fchdir(int fd) {
    struct process *process = process_current();
    if (!process || fd < 0 || fd >= PROCESS_MAX_FDS || !process->fds[fd]) return -EBADF;
    struct file *file = process->fds[fd];
    if (file->kind != FILE_KIND_VFS || !file->node || (file->node->flags & 0xFFU) != VFS_DIRECTORY) return -ENOTDIR;
    process->cwd = file->node;
    return 0;
}

static int64_t sys_mkdir_at(int dirfd, uint64_t user_path, uint64_t mode) {
    char path[256];
    int status = copy_path_at(dirfd, user_path, path);
    if (status != 0) return status;
    if (vfs_lookup(path)) return -EEXIST;
    return vfs_create_directory(path, mode_after_umask(mode)) ? 0 : -ENOENT;
}

static int64_t sys_readlink_at(int dirfd, uint64_t user_path, uint64_t user_buffer, size_t size) {
    char path[256];
    int status = copy_path_at(dirfd, user_path, path);
    if (status != 0) return status;
    const char *special_target = NULL;
    if (strcmp(path, "/proc/self/exe") == 0) special_target = process_current()->exe_path;
    else if (strcmp(path, "/proc/thread-self/exe") == 0) special_target = process_current()->exe_path;
    if (special_target) {
        size_t length = strlen(special_target);
        if (length > size) length = size;
        return copy_to_user(user_buffer, special_target, length) == 0 ? (int64_t)length : -EFAULT;
    }

    struct vfs_node *node = vfs_lookup_nofollow(path);
    if (!node) return -ENOENT;
    if ((node->flags & 0xFFU) != VFS_SYMLINK) return -EINVAL;
    char target[256];
    int64_t length = vfs_readlink(node, target, sizeof(target));
    if (length < 0) return -EINVAL;
    if ((size_t)length > size) length = (int64_t)size;
    return copy_to_user(user_buffer, target, (size_t)length) == 0 ? length : -EFAULT;
}

static int64_t sys_unlink_at(int dirfd, uint64_t user_path, int flags) {
    if (flags & ~AT_REMOVEDIR) return -EINVAL;
    char path[256];
    int status = copy_path_at(dirfd, user_path, path);
    if (status != 0) return status;
    struct vfs_node *node = vfs_lookup_nofollow(path);
    if (!node) return -ENOENT;
    uint32_t kind = node->flags & 0xFFU;
    if (node->flags & VFS_READONLY) return -EROFS;
    if ((flags & AT_REMOVEDIR) && kind != VFS_DIRECTORY) return -ENOTDIR;
    if (!(flags & AT_REMOVEDIR) && kind == VFS_DIRECTORY) return -EISDIR;
    if ((flags & AT_REMOVEDIR) && node->children) return -ENOTEMPTY;
    return vfs_remove(path, (flags & AT_REMOVEDIR) != 0) == 0 ? 0 : -EIO;
}

static int64_t sys_rename_at(int old_dirfd, uint64_t user_old_path,
                             int new_dirfd, uint64_t user_new_path,
                             unsigned flags) {
    if (flags != 0) return -EINVAL;
    char old_path[256], new_path[256];
    int status = copy_path_at(old_dirfd, user_old_path, old_path);
    if (status != 0) return status;
    status = copy_path_at(new_dirfd, user_new_path, new_path);
    if (status != 0) return status;
    struct vfs_node *node = vfs_lookup_nofollow(old_path);
    if (!node) return -ENOENT;
    if (node->flags & VFS_READONLY) return -EROFS;
    return vfs_rename(old_path, new_path) == 0 ? 0 : -EIO;
}

static int64_t sys_symlink_at(uint64_t user_target, int new_dirfd,
                              uint64_t user_link_path) {
    char target[256], link_path[256];
    if (copy_string_from_user(target, sizeof(target), user_target) < 0) return -EFAULT;
    int status = copy_path_at(new_dirfd, user_link_path, link_path);
    if (status != 0) return status;
    if (vfs_lookup_nofollow(link_path)) return -EEXIST;
    return vfs_create_symlink(link_path, target, 0) ? 0 : -EIO;
}

static int64_t sys_chmod_at(int dirfd, uint64_t user_path, uint32_t mode, int flags) {
    if (flags & ~AT_SYMLINK_NOFOLLOW) return -EINVAL;
    char path[256];
    int status = copy_path_at(dirfd, user_path, path);
    if (status != 0) return status;
    struct vfs_node *node = (flags & AT_SYMLINK_NOFOLLOW) ? vfs_lookup_nofollow(path) : vfs_lookup(path);
    if (!node) return -ENOENT;
    if (node->flags & VFS_READONLY) return -EROFS;
    node->mode = mode & 07777U;
    vfs_notify_meta_changed(node);
    return 0;
}

static int64_t sys_fchmod(int fd, uint32_t mode) {
    struct process *process = process_current();
    if (!process || fd < 0 || fd >= PROCESS_MAX_FDS || !process->fds[fd]) return -EBADF;
    struct file *file = process->fds[fd];
    if (file->kind != FILE_KIND_VFS || !file->node) return -EBADF;
    if (file->node->flags & VFS_READONLY) return -EROFS;
    file->node->mode = mode & 07777U;
    vfs_notify_meta_changed(file->node);
    return 0;
}

#define UTIME_NOW 0x3FFFFFFF
#define UTIME_OMIT 0x3FFFFFFE

static int64_t sys_utimens_at(int dirfd, uint64_t user_path, uint64_t user_times,
                              int flags) {
    if (flags & ~AT_SYMLINK_NOFOLLOW) return -EINVAL;

    struct vfs_node *node = NULL;
    if (!user_path) {
        /* Both musl and glibc implement futimens(fd, times) as
           utimensat(fd, NULL, times, 0), so a NULL path means "operate on
           dirfd itself" rather than being a bad pointer. */
        struct process *process = process_current();
        if (!process || dirfd < 0 || dirfd >= PROCESS_MAX_FDS || !process->fds[dirfd])
            return -EBADF;
        struct file *file = process->fds[dirfd];
        if (file->kind != FILE_KIND_VFS || !file->node) return -EBADF;
        node = file->node;
    } else {
        char path[256];
        int status = copy_path_at(dirfd, user_path, path);
        if (status != 0) return status;
        node = (flags & AT_SYMLINK_NOFOLLOW) ? vfs_lookup_nofollow(path)
                                             : vfs_lookup(path);
        if (!node) return -ENOENT;
    }
    if (node->flags & VFS_READONLY) return -EROFS;

    /* A NULL times array means "both to now"; musl also folds an explicit
       UTIME_NOW/UTIME_NOW pair into that form before issuing the syscall. */
    if (!user_times) {
        vfs_stamp_times(node, VFS_TIME_ATIME | VFS_TIME_MTIME | VFS_TIME_CTIME);
        vfs_notify_meta_changed(node);
        return 0;
    }

    struct linux_timespec times[2];
    if (copy_from_user(times, user_times, sizeof(times)) != 0) return -EFAULT;

    uint32_t now = (uint32_t)time_epoch_seconds();
    for (int index = 0; index < 2; index++) {
        int64_t nsec = times[index].tv_nsec;
        if (nsec == UTIME_OMIT) continue;
        uint32_t value = (nsec == UTIME_NOW) ? now : (uint32_t)times[index].tv_sec;
        if (index == 0) node->atime = value;
        else node->mtime = value;
    }
    /* Changing the times is itself a metadata change. */
    node->ctime = now;
    vfs_notify_meta_changed(node);
    return 0;
}

static int map_zero_pages(struct process *process, uint64_t start, uint64_t end, uint64_t flags) {
    for (uint64_t address = start; address < end; address += 4096) {
        if (vmm_translate(process->cr3, address, NULL, NULL) == 0) continue;
        uint64_t physical = (uint64_t)pmm_alloc_page();
        if (!physical) return -1;
        memset(vmm_phys_to_virt(physical), 0, 4096);
        if (vmm_map_page_in(process->cr3, address, physical, flags | PAGE_USER | PAGE_PRESENT) != 0) {
            pmm_free_page((void *)physical);
            return -1;
        }
    }
    return 0;
}

static void unmap_pages(struct process *process, uint64_t start, uint64_t end) {
    for (uint64_t address = start; address < end; address += 4096) {
        uint64_t physical;
        uint64_t flags;
        if (vmm_translate(process->cr3, address, &physical, &flags) == 0) {
            vmm_unmap_page_in(process->cr3, address);
            if (!(flags & PAGE_DEVICE)) pmm_free_page((void *)(physical & ~0xFFFULL));
        }
    }
}

static int64_t sys_brk(uint64_t requested) {
    struct process *process = process_current();
    if (!process) return -EINVAL;
    struct process_memory *memory = process->memory;
    uint64_t brk_start = memory ? memory->brk_start : process->brk_start;
    uint64_t brk_end = memory ? memory->brk_end : process->brk_end;
    if (requested == 0) return (int64_t)brk_end;
    if (requested < brk_start || requested >= USER_BRK_LIMIT) return (int64_t)brk_end;
    uint64_t old_page_end = align_up(brk_end, 4096);
    uint64_t new_page_end = align_up(requested, 4096);
    if (new_page_end > old_page_end) {
        if (map_zero_pages(process, old_page_end, new_page_end, PAGE_WRITE) != 0) return (int64_t)brk_end;
    } else if (new_page_end < old_page_end) unmap_pages(process, new_page_end, old_page_end);
    process->brk_end = requested;
    if (memory) memory->brk_end = requested;
    return (int64_t)requested;
}

static int mapping_range_free(struct process *process, uint64_t base, uint64_t length) {
    if (!process || !length || base >= USER_ADDRESS_LIMIT ||
        length > USER_ADDRESS_LIMIT - base) return 0;
    for (uint64_t page = base; page < base + length; page += 4096) {
        if (vmm_translate(process->cr3, page, NULL, NULL) == 0) return 0;
    }
    return 1;
}

static int find_mapping_range(struct process *process, uint64_t start,
                              uint64_t length, uint64_t *base_out) {
    uint64_t base = align_up(start, 4096);
    while (base < USER_ADDRESS_LIMIT && length <= USER_ADDRESS_LIMIT - base) {
        if (mapping_range_free(process, base, length)) {
            *base_out = base;
            return 0;
        }
        if (USER_ADDRESS_LIMIT - base < length + 4096ULL) break;
        base += 4096;
    }
    return -1;
}

static int64_t sys_mmap(uint64_t address, uint64_t length, int prot, int flags, int fd, uint64_t offset) {
    struct process *process = process_current();
    if (!process || !length) return -EINVAL;
    if ((flags & MAP_SHARED) && (flags & MAP_PRIVATE)) return -EINVAL;
    if (!(flags & MAP_SHARED) && !(flags & MAP_PRIVATE)) return -EINVAL;
    if (!(flags & MAP_ANONYMOUS) && (offset & 0xFFFULL)) return -EINVAL;
    if (length > UINT64_MAX - 4095ULL) return -EINVAL;
    length = align_up(length, 4096);

    int fixed = (flags & MAP_FIXED) != 0;
    int no_replace = (flags & MAP_FIXED_NOREPLACE) != 0;
    if (fixed && no_replace) return -EINVAL;

    uint64_t base;
    int advance_mmap_base = 0;
    if (fixed || no_replace) {
        if (address & 0xFFFULL) return -EINVAL;
        base = address;
    } else if (address) {
        uint64_t hint = align_up(address, 4096);
        if (mapping_range_free(process, hint, length)) {
            base = hint;
        } else {
            uint64_t start = process->memory ? process->memory->mmap_base : process->mmap_base;
            if (find_mapping_range(process, start, length, &base) != 0) return -ENOMEM;
            advance_mmap_base = 1;
        }
    } else {
        uint64_t start = process->memory ? process->memory->mmap_base : process->mmap_base;
        if (find_mapping_range(process, start, length, &base) != 0) return -ENOMEM;
        advance_mmap_base = 1;
    }
    if (base < 0x10000ULL || base >= USER_ADDRESS_LIMIT ||
        length > USER_ADDRESS_LIMIT - base) return -EINVAL;

    if (no_replace && !mapping_range_free(process, base, length)) return -EEXIST;
    if (fixed) unmap_pages(process, base, base + length);

    uint64_t page_flags = (prot & PROT_WRITE) ? PAGE_WRITE : 0;
    if (nx_enabled && !(prot & PROT_EXEC)) page_flags |= PAGE_NX;

    struct file *file = NULL;
    if (!(flags & MAP_ANONYMOUS)) {
        if (fd < 0 || fd >= PROCESS_MAX_FDS || !process->fds[fd]) return -EBADF;
        file = process->fds[fd];
        if ((file->kind != FILE_KIND_VFS && file->kind != FILE_KIND_FRAMEBUFFER) ||
            !file->node) return -ENODEV;
        if (file->node->mmap) {
            if (!(flags & MAP_SHARED)) return -EINVAL;
            int64_t status = file->node->mmap(file->node, file, process->cr3,
                                              base, length, offset, page_flags);
            if (status < 0) return status;
            if (advance_mmap_base) {
                process->mmap_base = base + length + 4096;
                if (process->memory) process->memory->mmap_base = process->mmap_base;
            }
            return (int64_t)base;
        }
    }

    uint64_t allocation_flags = file ? (page_flags | PAGE_WRITE) : page_flags;
    if (map_zero_pages(process, base, base + length, allocation_flags) != 0) {
        unmap_pages(process, base, base + length);
        return -ENOMEM;
    }

    if (file) {
        uint8_t buffer[256];
        uint64_t copied = 0;
        while (copied < length) {
            size_t chunk = length - copied > sizeof(buffer) ? sizeof(buffer) :
                           (size_t)(length - copied);
            int64_t amount = vfs_read(file->node, offset + copied, chunk, buffer);
            if (amount < 0) {
                unmap_pages(process, base, base + length);
                return amount;
            }
            if (amount == 0) break;
            if (vmm_copy_to_space(process->cr3, base + copied, buffer,
                                  (size_t)amount) != 0) {
                unmap_pages(process, base, base + length);
                return -EFAULT;
            }
            copied += (uint64_t)amount;
            if ((size_t)amount < chunk) break;
        }
    }
    if (file && !(prot & PROT_WRITE)) {
        uint64_t final_flags = PAGE_USER | PAGE_PRESENT | page_flags;
        for (uint64_t page = base; page < base + length; page += 4096) {
            if (vmm_protect_page_in(process->cr3, page, final_flags) != 0) {
                unmap_pages(process, base, base + length);
                return -ENOMEM;
            }
        }
    }
    if (advance_mmap_base) {
        process->mmap_base = base + length + 4096;
        if (process->memory) process->memory->mmap_base = process->mmap_base;
    }
    return (int64_t)base;
}

static int64_t sys_munmap(uint64_t address, uint64_t length) {
    struct process *process = process_current();
    if (!process || (address & 0xFFFULL) || !length) return -EINVAL;
    length = align_up(length, 4096);
    if (address >= USER_ADDRESS_LIMIT || length > USER_ADDRESS_LIMIT - address) return -EINVAL;
    unmap_pages(process, address, address + length);
    return 0;
}

static int64_t sys_mprotect(uint64_t address, uint64_t length, int prot) {
    struct process *process = process_current();
    if (!process || (address & 0xFFFULL) || !length) return -EINVAL;
    length = align_up(length, 4096);
    uint64_t flags = PAGE_USER | PAGE_PRESENT;
    if (prot & PROT_WRITE) flags |= PAGE_WRITE;
    if (nx_enabled && !(prot & PROT_EXEC)) flags |= PAGE_NX;
    for (uint64_t page = address; page < address + length; page += 4096) {
        uint64_t old_flags;
        if (vmm_translate(process->cr3, page, NULL, &old_flags) != 0) return -ENOMEM;
        uint64_t effective_flags = flags | (old_flags & PAGE_DEVICE);
        if (nx_enabled && (old_flags & PAGE_DEVICE)) effective_flags |= PAGE_NX;
        if (vmm_protect_page_in(process->cr3, page, effective_flags) != 0) return -ENOMEM;
    }
    return 0;
}

static int copy_exec_vector(uint64_t user_vector, char storage[MAX_EXEC_ITEMS][MAX_EXEC_STRING],
                            const char *pointers[MAX_EXEC_ITEMS + 1]) {
    if (!user_vector) {
        pointers[0] = NULL;
        return 0;
    }
    for (int index = 0; index < MAX_EXEC_ITEMS; index++) {
        uint64_t user_string;
        if (copy_from_user(&user_string, user_vector + (uint64_t)index * sizeof(uint64_t), sizeof(user_string)) != 0) return -EFAULT;
        if (!user_string) {
            pointers[index] = NULL;
            return index;
        }
        if (copy_string_from_user(storage[index], MAX_EXEC_STRING, user_string) < 0) return -EFAULT;
        pointers[index] = storage[index];
    }
    return -E2BIG;
}

static int parse_shebang(struct vfs_node *file, char interpreter[MAX_EXEC_STRING],
                          char optional_argument[MAX_EXEC_STRING]) {
    unsigned char header[MAX_EXEC_STRING];
    if (!file || (file->flags & 0xFFU) != VFS_FILE || file->length < 2) return 0;
    size_t amount = file->length < sizeof(header) - 1 ? (size_t)file->length : sizeof(header) - 1;
    int64_t read = vfs_read(file, 0, amount, header);
    if (read < 2 || header[0] != '#' || header[1] != '!') return 0;
    header[read] = '\0';

    size_t at = 2;
    while (at < (size_t)read && (header[at] == ' ' || header[at] == '\t')) at++;
    size_t start = at;
    while (at < (size_t)read && header[at] != ' ' && header[at] != '\t' &&
           header[at] != '\n' && header[at] != '\r') at++;
    size_t length = at - start;
    if (!length || length >= MAX_EXEC_STRING || header[start] != '/') return -ENOEXEC;
    memcpy(interpreter, header + start, length);
    interpreter[length] = '\0';

    while (at < (size_t)read && (header[at] == ' ' || header[at] == '\t')) at++;
    start = at;
    while (at < (size_t)read && header[at] != '\n' && header[at] != '\r') at++;
    while (at > start && (header[at - 1] == ' ' || header[at - 1] == '\t')) at--;
    length = at - start;
    if (length >= MAX_EXEC_STRING) return -ENOEXEC;
    if (length) memcpy(optional_argument, header + start, length);
    optional_argument[length] = '\0';
    return 1;
}

static int rewrite_script_arguments(struct exec_arguments *arguments, int argc,
                                    const char *script_path, const char *interpreter,
                                    const char *optional_argument) {
    int has_optional = optional_argument && optional_argument[0];
    int prefix = has_optional ? 3 : 2;
    int original_tail = argc > 0 ? argc - 1 : 0;
    int new_argc = prefix + original_tail;
    if (new_argc > MAX_EXEC_ITEMS) return -E2BIG;

    for (int index = original_tail - 1; index >= 0; index--) {
        int source = index + 1;
        int destination = prefix + index;
        strncpy(arguments->argv_storage[destination], arguments->argv_storage[source],
                MAX_EXEC_STRING - 1);
        arguments->argv_storage[destination][MAX_EXEC_STRING - 1] = '\0';
    }

    strncpy(arguments->argv_storage[0], interpreter, MAX_EXEC_STRING - 1);
    arguments->argv_storage[0][MAX_EXEC_STRING - 1] = '\0';
    int script_index = 1;
    if (has_optional) {
        strncpy(arguments->argv_storage[1], optional_argument, MAX_EXEC_STRING - 1);
        arguments->argv_storage[1][MAX_EXEC_STRING - 1] = '\0';
        script_index = 2;
    }
    strncpy(arguments->argv_storage[script_index], script_path, MAX_EXEC_STRING - 1);
    arguments->argv_storage[script_index][MAX_EXEC_STRING - 1] = '\0';

    for (int index = 0; index < new_argc; index++)
        arguments->argv[index] = arguments->argv_storage[index];
    arguments->argv[new_argc] = NULL;
    return new_argc;
}

static int64_t sys_execve(struct syscall_frame *frame, uint64_t user_path, uint64_t user_argv, uint64_t user_envp) {
    char path[256];
    int status = copy_path_at(AT_FDCWD, user_path, path);
    if (status != 0) return status;
    struct exec_arguments *arguments = (struct exec_arguments *)kmalloc(sizeof(*arguments));
    if (!arguments) return -ENOMEM;
    memset(arguments, 0, sizeof(*arguments));
    int argc = copy_exec_vector(user_argv, arguments->argv_storage, arguments->argv);
    int envc = copy_exec_vector(user_envp, arguments->env_storage, arguments->envp);
    if (argc < 0 || envc < 0) {
        kfree(arguments);
        return argc < 0 ? argc : envc;
    }
    if (argc == 0) {
        strncpy(arguments->argv_storage[0], path, MAX_EXEC_STRING - 1);
        arguments->argv[0] = arguments->argv_storage[0];
        arguments->argv[1] = NULL;
        argc = 1;
    }
    if (envc == 0) {
        const char *defaults[] = {"PATH=/usr/bin:/usr/sbin:/bin:/sbin", "HOME=/", "TERM=tunix", "USER=root", NULL};
        for (int i = 0; defaults[i]; i++) {
            strncpy(arguments->env_storage[i], defaults[i], MAX_EXEC_STRING - 1);
            arguments->envp[i] = arguments->env_storage[i];
            arguments->envp[i + 1] = NULL;
        }
    }

    struct vfs_node *file = vfs_lookup(path);
    if (!file) {
        kfree(arguments);
        return -ENOENT;
    }
    if ((file->flags & 0xFFU) != VFS_FILE) {
        kfree(arguments);
        return -EACCES;
    }
    if ((file->mode & 0111U) == 0) {
        kfree(arguments);
        return -EACCES;
    }

    char interpreter[MAX_EXEC_STRING];
    char optional_argument[MAX_EXEC_STRING];
    int script = parse_shebang(file, interpreter, optional_argument);
    if (script < 0) {
        kfree(arguments);
        return script;
    }
    if (script > 0) {
        struct vfs_node *interpreter_file = vfs_lookup(interpreter);
        if (!interpreter_file || (interpreter_file->flags & 0xFFU) != VFS_FILE ||
            (interpreter_file->mode & 0111U) == 0) {
            kfree(arguments);
            return -ENOENT;
        }
        int rewritten = rewrite_script_arguments(arguments, argc, path, interpreter,
                                                 optional_argument);
        if (rewritten < 0) {
            kfree(arguments);
            return rewritten;
        }
        int64_t result = process_exec_from_syscall(frame, interpreter,
                                                   arguments->argv, arguments->envp);
        kfree(arguments);
        return result == -1 ? -ENOEXEC : result;
    }

    int64_t result = process_exec_from_syscall(frame, path, arguments->argv, arguments->envp);
    kfree(arguments);
    return result == -1 ? -ENOEXEC : result;
}

static int64_t sys_sigaction(int signal_number, uint64_t user_action, uint64_t user_old_action, uint64_t sigset_size) {
    if (sigset_size != 8 && sigset_size != 0) return -EINVAL;
    struct process *process = process_current();
    if (!process || signal_number < 1 || signal_number > TUNIX_NSIG || signal_number == SIGKILL) return -EINVAL;
    struct tunix_sigaction *slot = &process->signal_actions[signal_number - 1];
    if (user_old_action && copy_to_user(user_old_action, slot, sizeof(*slot)) != 0) return -EFAULT;
    if (user_action) {
        struct tunix_sigaction action;
        if (copy_from_user(&action, user_action, sizeof(action)) != 0) return -EFAULT;
        *slot = action;
    }
    return 0;
}

static int64_t sys_sigprocmask(int how, uint64_t user_set, uint64_t user_old_set, uint64_t sigset_size) {
    if (sigset_size != 8 && sigset_size != 0) return -EINVAL;
    struct process *process = process_current();
    if (!process) return -EINVAL;
    if (user_old_set && copy_to_user(user_old_set, &process->signal_blocked, sizeof(process->signal_blocked)) != 0) return -EFAULT;
    if (!user_set) return 0;
    uint64_t set;
    if (copy_from_user(&set, user_set, sizeof(set)) != 0) return -EFAULT;
    set &= ~(1ULL << (SIGKILL - 1));
    if (how == SIG_BLOCK) process->signal_blocked |= set;
    else if (how == SIG_UNBLOCK) process->signal_blocked &= ~set;
    else if (how == SIG_SETMASK) process->signal_blocked = set;
    else return -EINVAL;
    return 0;
}

/*
 * Rewind the syscall so it re-runs and re-tests its condition, then block.
 * Sleeping requires a wakeup source for this file kind; without one the only
 * option is to spin, which is why the yield is a fallback rather than the rule.
 */
static void block_and_retry(struct syscall_frame *frame, uint64_t syscall_number,
                            struct file *file, int writing) {
    frame->user_rip -= 2U;
    frame->rax = syscall_number;
    struct process *process = process_current();
    if (process) process->syscall_rewound = 1;
    const void *channel = writing ? file_write_wait_channel(file)
                                  : file_read_wait_channel(file);
    if (!channel || process_sleep_on(frame, channel) != 0)
        process_yield_from_syscall(frame);
}

static int64_t sys_readv_writev(int fd, uint64_t user_iov, int count, int write_mode) {
    if (count < 0 || count > 1024) return -EINVAL;
    int64_t total = 0;
    for (int i = 0; i < count; i++) {
        struct linux_iovec iov;
        if (copy_from_user(&iov, user_iov + (uint64_t)i * sizeof(iov), sizeof(iov)) != 0) return total ? total : -EFAULT;
        int64_t result = write_mode ? sys_write(fd, iov.base, (size_t)iov.length) : sys_read(fd, iov.base, (size_t)iov.length);
        if (result < 0) return total ? total : result;
        total += result;
        if ((uint64_t)result < iov.length) break;
    }
    return total;
}

static int64_t sys_uname(uint64_t user_buffer) {
    struct linux_utsname value;
    memset(&value, 0, sizeof(value));
    strncpy(value.sysname, "Tunix", sizeof(value.sysname) - 1);
    strncpy(value.nodename, "tunix", sizeof(value.nodename) - 1);
    strncpy(value.release, "0.1.0", sizeof(value.release) - 1);
    strncpy(value.version, "Tunix Kernel", sizeof(value.version) - 1);
    strncpy(value.machine, "x86_64", sizeof(value.machine) - 1);
    strncpy(value.domainname, "localdomain", sizeof(value.domainname) - 1);
    return copy_to_user(user_buffer, &value, sizeof(value)) == 0 ? 0 : -EFAULT;
}

#define CLOCK_REALTIME 0
#define CLOCK_MONOTONIC 1
#define CLOCK_MONOTONIC_RAW 4
#define CLOCK_REALTIME_COARSE 5
#define CLOCK_MONOTONIC_COARSE 6
#define CLOCK_BOOTTIME 7

#define GRND_NONBLOCK 0x0001U
#define GRND_RANDOM 0x0002U
#define GRND_INSECURE 0x0004U

static int64_t sys_clock_gettime(int clock_id, uint64_t user_time) {
    uint64_t now;
    switch (clock_id) {
        case CLOCK_REALTIME:
        case CLOCK_REALTIME_COARSE:
            now = time_realtime_ns();
            break;
        case CLOCK_MONOTONIC:
        case CLOCK_MONOTONIC_RAW:
        case CLOCK_MONOTONIC_COARSE:
        case CLOCK_BOOTTIME:
            now = time_uptime_ns();
            break;
        default:
            return -EINVAL;
    }
    struct linux_timespec value = {(int64_t)(now / 1000000000ULL),
                                    (int64_t)(now % 1000000000ULL)};
    return copy_to_user(user_time, &value, sizeof(value)) == 0 ? 0 : -EFAULT;
}

static int64_t sys_gettimeofday(uint64_t user_time) {
    if (!user_time) return 0;
    uint64_t now = time_realtime_ns();
    struct linux_timeval value = {(int64_t)(now / 1000000000ULL),
                                   (int64_t)((now % 1000000000ULL) / 1000ULL)};
    return copy_to_user(user_time, &value, sizeof(value)) == 0 ? 0 : -EFAULT;
}

static int64_t sys_getrandom(uint64_t user_buffer, size_t length, unsigned flags) {
    if (flags & ~(GRND_NONBLOCK | GRND_RANDOM | GRND_INSECURE)) return -EINVAL;
    uint8_t bytes[64];
    size_t completed = 0;
    while (completed < length) {
        size_t chunk = length - completed;
        if (chunk > sizeof(bytes)) chunk = sizeof(bytes);
        random_get_bytes(bytes, chunk);
        if (copy_to_user(user_buffer + completed, bytes, chunk) != 0) {
            memset(bytes, 0, sizeof(bytes));
            return completed ? (int64_t)completed : -EFAULT;
        }
        completed += chunk;
    }
    memset(bytes, 0, sizeof(bytes));
    return (int64_t)completed;
}

static int64_t sys_arch_prctl(int code, uint64_t address) {
    if (code == ARCH_SET_FS) {
        if (address >= USER_ADDRESS_LIMIT) return -EINVAL;
        process_set_fs_base(address);
        return 0;
    }
    if (code == ARCH_GET_FS) {
        uint64_t value = process_get_fs_base();
        return copy_to_user(address, &value, sizeof(value)) == 0 ? 0 : -EFAULT;
    }
    return -EINVAL;
}

static int signal_stack_active(const struct process *process, uint64_t user_rsp) {
    if (!process || process->signal_stack_flags == SS_DISABLE ||
        !process->signal_stack_size) return 0;
    uint64_t base = process->signal_stack_pointer;
    uint64_t limit = base + process->signal_stack_size;
    return user_rsp >= base && user_rsp < limit;
}

static int64_t sys_sigaltstack(struct syscall_frame *frame, uint64_t user_stack,
                               uint64_t user_old_stack) {
    struct process *process = process_current();
    if (!process || !frame) return -EINVAL;
    int on_stack = signal_stack_active(process, frame->user_rsp);

    if (user_old_stack) {
        struct linux_sigaltstack old_stack;
        old_stack.sp = process->signal_stack_pointer;
        old_stack.size = process->signal_stack_size;
        old_stack.__pad = 0;
        old_stack.flags = process->signal_stack_flags == SS_DISABLE
            ? SS_DISABLE : (on_stack ? SS_ONSTACK : 0);
        if (copy_to_user(user_old_stack, &old_stack, sizeof(old_stack)) != 0)
            return -EFAULT;
    }

    if (!user_stack) return 0;
    if (on_stack) return -EPERM;

    struct linux_sigaltstack new_stack;
    if (copy_from_user(&new_stack, user_stack, sizeof(new_stack)) != 0) return -EFAULT;
    if (new_stack.flags & ~(SS_DISABLE)) return -EINVAL;

    if (new_stack.flags & SS_DISABLE) {
        process->signal_stack_pointer = 0;
        process->signal_stack_size = 0;
        process->signal_stack_flags = SS_DISABLE;
        return 0;
    }

    if (new_stack.size < MINSIGSTKSZ) return -ENOMEM;
    if (new_stack.sp >= USER_ADDRESS_LIMIT ||
        new_stack.size > USER_ADDRESS_LIMIT - new_stack.sp) return -EINVAL;
    process->signal_stack_pointer = new_stack.sp;
    process->signal_stack_size = new_stack.size;
    process->signal_stack_flags = 0;
    return 0;
}

static int64_t sys_prctl(int option, uint64_t arg2, uint64_t arg3,
                         uint64_t arg4, uint64_t arg5) {
    struct process *process = process_current();
    if (!process) return -EINVAL;

    switch (option) {
        case PR_SET_PDEATHSIG:
            if (arg2 > TUNIX_NSIG) return -EINVAL;
            process->pdeath_signal = (int)arg2;
            return 0;
        case PR_GET_PDEATHSIG: {
            if (!arg2) return -EFAULT;
            int value = process->pdeath_signal;
            return copy_to_user(arg2, &value, sizeof(value)) == 0 ? 0 : -EFAULT;
        }
        case PR_GET_DUMPABLE:
            return process->dumpable;
        case PR_SET_DUMPABLE:
            if (arg2 > 1) return -EINVAL;
            process->dumpable = (int)arg2;
            return 0;
        case PR_SET_NAME: {
            if (!arg2) return -EFAULT;
            char name[16];
            if (copy_from_user(name, arg2, sizeof(name)) != 0) return -EFAULT;
            name[sizeof(name) - 1] = '\0';
            memset(process->name, 0, sizeof(process->name));
            strncpy(process->name, name, sizeof(process->name) - 1);
            return 0;
        }
        case PR_GET_NAME: {
            if (!arg2) return -EFAULT;
            char name[16];
            memset(name, 0, sizeof(name));
            strncpy(name, process->name, sizeof(name) - 1);
            return copy_to_user(arg2, name, sizeof(name)) == 0 ? 0 : -EFAULT;
        }
        case PR_SET_NO_NEW_PRIVS:
            if (arg2 != 1 || arg3 || arg4 || arg5) return -EINVAL;
            process->no_new_privs = 1;
            return 0;
        case PR_GET_NO_NEW_PRIVS:
            return process->no_new_privs;
        case PR_GET_TID_ADDRESS:
            if (!arg2) return -EFAULT;
            return copy_to_user(arg2, &process->clear_child_tid_user,
                                sizeof(process->clear_child_tid_user)) == 0 ? 0 : -EFAULT;
        case PR_SET_CHILD_SUBREAPER:
            process->child_subreaper = arg2 != 0;
            return 0;
        case PR_GET_CHILD_SUBREAPER: {
            if (!arg2) return -EFAULT;
            int value = process->child_subreaper;
            return copy_to_user(arg2, &value, sizeof(value)) == 0 ? 0 : -EFAULT;
        }
        case PR_SET_THP_DISABLE:
            if (arg2 > 1) return -EINVAL;
            process->thp_disable = (int)arg2;
            return 0;
        case PR_GET_THP_DISABLE:
            return process->thp_disable;
        case PR_SET_TIMERSLACK:
            process->timerslack_ns = arg2 ? arg2 : 50000ULL;
            return 0;
        case PR_GET_TIMERSLACK:
            return (int64_t)process->timerslack_ns;
        case PR_GET_KEEPCAPS:
        case PR_GET_SECUREBITS:
        case PR_GET_SECCOMP:
            return 0;
        case PR_SET_KEEPCAPS:
        case PR_SET_SECUREBITS:
            return arg2 == 0 ? 0 : -EINVAL;
        case PR_CAPBSET_READ:
            return 0;
        case PR_CAPBSET_DROP:
        case PR_SET_PTRACER:
            return 0;
        case PR_CAP_AMBIENT:
            if (arg2 == PR_CAP_AMBIENT_IS_SET) return 0;
            if (arg2 == PR_CAP_AMBIENT_LOWER || arg2 == PR_CAP_AMBIENT_CLEAR_ALL) return 0;
            if (arg2 == PR_CAP_AMBIENT_RAISE) return -EPERM;
            return -EINVAL;
        default:
            return -EINVAL;
    }
}

static int64_t sys_set_robust_list(uint64_t user_head, size_t length) {
    struct process *process = process_current();
    if (!process) return -EINVAL;
    if (length != 24U) return -EINVAL;
    if (user_head >= USER_ADDRESS_LIMIT) return -EFAULT;
    process->robust_list_head = user_head;
    process->robust_list_length = length;
    return 0;
}

static int64_t sys_get_robust_list(int pid, uint64_t user_head_pointer,
                                   uint64_t user_length_pointer) {
    if (!user_head_pointer || !user_length_pointer) return -EFAULT;
    struct process *target = pid == 0 ? process_current() : process_find((uint64_t)pid);
    if (!target) return -ESRCH;
    uint64_t head = target->robust_list_head;
    uint64_t length = target->robust_list_length;
    if (copy_to_user(user_head_pointer, &head, sizeof(head)) != 0) return -EFAULT;
    return copy_to_user(user_length_pointer, &length, sizeof(length)) == 0 ? 0 : -EFAULT;
}

static int64_t sys_prlimit(uint64_t user_old_limit) {
    if (!user_old_limit) return 0;
    struct linux_rlimit value = {UINT64_MAX, UINT64_MAX};
    return copy_to_user(user_old_limit, &value, sizeof(value)) == 0 ? 0 : -EFAULT;
}

static int64_t sys_clone_fork_compat(struct syscall_frame *frame,
                                     uint64_t flags, uint64_t child_stack,
                                     uint64_t parent_tid_user, uint64_t child_tid_user,
                                     uint64_t tls) {
    uint64_t exit_signal = flags & 0xFFULL;
    uint64_t thread_allowed = CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND |
                              CLONE_THREAD | CLONE_SYSVSEM | CLONE_SETTLS | CLONE_DETACHED |
                              CLONE_FORK_METADATA_FLAGS;
    if (flags & CLONE_THREAD) {
        uint64_t unsupported = flags & ~(0xFFULL | thread_allowed);
        if (unsupported || !(flags & CLONE_VM) || !(flags & CLONE_SIGHAND) ||
            !child_stack || exit_signal != 0) return -EINVAL;
        return process_clone_thread_from_syscall(frame, child_stack, tls,
                                                 parent_tid_user, child_tid_user, flags);
    }

    uint64_t unsupported = flags & ~(0xFFULL | CLONE_FORK_METADATA_FLAGS);
    if (child_stack != 0 || unsupported != 0 || (flags & CLONE_FORK_REJECT_FLAGS) != 0) {
        KDEBUG("syscall: clone unsupported flags=0x%llx stack=0x%llx\n",
                (unsigned long long)flags, (unsigned long long)child_stack);
        return -ENOSYS;
    }
    if (exit_signal != 0 && exit_signal != SIGCHLD) return -EINVAL;

    int64_t pid = process_fork_from_syscall(frame);
    if (pid <= 0) return pid;

    uint32_t tid = (uint32_t)pid;
    if ((flags & CLONE_PARENT_SETTID) && parent_tid_user) {
        if (copy_to_user(parent_tid_user, &tid, sizeof(tid)) != 0) return -EFAULT;
    }

    struct process *child = process_find((uint64_t)pid);
    if (!child) return -ESRCH;
    if ((flags & CLONE_CHILD_SETTID) && child_tid_user) {
        if (vmm_copy_to_space(child->cr3, child_tid_user, &tid, sizeof(tid)) != 0)
            return -EFAULT;
    }
    if ((flags & CLONE_CHILD_CLEARTID) && child_tid_user)
        child->clear_child_tid_user = child_tid_user;

    return pid;
}

static int64_t sys_clone3_fork_compat(struct syscall_frame *frame,
                                      uint64_t user_args, size_t size) {
    struct linux_clone_args args;
    if (size < 64 || size > sizeof(args)) return -EINVAL;
    memset(&args, 0, sizeof(args));
    if (copy_from_user(&args, user_args, size) != 0) return -EFAULT;
    if (args.pidfd || args.set_tid || args.set_tid_size || args.cgroup)
        return -ENOSYS;
    uint64_t flags = args.flags | (args.exit_signal & 0xFFULL);
    return sys_clone_fork_compat(frame, flags, args.stack_size ? args.stack + args.stack_size : args.stack, args.parent_tid, args.child_tid, args.tls);
}


static struct file *file_from_fd(int fd) {
    struct process *process = process_current();
    if (!process || fd < 0 || fd >= PROCESS_MAX_FDS) return NULL;
    return process->fds[fd];
}

static int install_new_file(struct file *file, int cloexec) {
    if (!file) return -ENOMEM;
    int fd = process_install_file_flags(process_current(), file, 0,
        cloexec ? PROCESS_FD_CLOEXEC : 0);
    if (fd < 0) {
        file_unref(file);
        return -EMFILE;
    }
    return fd;
}

static int64_t sys_eventfd(uint64_t initial_value, int flags, int legacy) {
    int supported = EFD_SEMAPHORE | EFD_NONBLOCK | EFD_CLOEXEC;
    if (flags & ~supported) return -EINVAL;
    if (legacy && flags) return -EINVAL;
    struct eventfd_context *context = eventfd_create(initial_value,
                                                     flags & EFD_SEMAPHORE);
    if (!context) return -ENOMEM;
    struct file *file = file_create_eventfd(context,
        (uint32_t)(flags & EFD_NONBLOCK));
    if (!file) {
        eventfd_destroy(context);
        return -ENOMEM;
    }
    return install_new_file(file, flags & EFD_CLOEXEC);
}

static int64_t sys_timerfd_create(int clock_id, int flags) {
    if (flags & ~(TFD_NONBLOCK | TFD_CLOEXEC)) return -EINVAL;
    struct timerfd_context *context = timerfd_create(clock_id);
    if (!context) return -EINVAL;
    struct file *file = file_create_timerfd(context,
        (uint32_t)(flags & TFD_NONBLOCK));
    if (!file) {
        timerfd_destroy(context);
        return -ENOMEM;
    }
    return install_new_file(file, flags & TFD_CLOEXEC);
}

static int64_t sys_timerfd_settime(int fd, int flags, uint64_t user_new,
                                   uint64_t user_old) {
    if (!user_new || (flags & ~TFD_TIMER_ABSTIME)) return -EINVAL;
    struct file *file = file_from_fd(fd);
    if (!file || file->kind != FILE_KIND_TIMERFD) return -EBADF;
    struct tunix_itimerspec new_value;
    struct tunix_itimerspec old_value;
    if (copy_from_user(&new_value, user_new, sizeof(new_value)) != 0) return -EFAULT;
    int status = timerfd_settime(file->timerfd, flags, &new_value,
                                 user_old ? &old_value : NULL);
    if (status < 0) return status;
    if (user_old && copy_to_user(user_old, &old_value, sizeof(old_value)) != 0)
        return -EFAULT;
    return 0;
}

static int64_t sys_timerfd_gettime(int fd, uint64_t user_value) {
    if (!user_value) return -EFAULT;
    struct file *file = file_from_fd(fd);
    if (!file || file->kind != FILE_KIND_TIMERFD) return -EBADF;
    struct tunix_itimerspec value;
    int status = timerfd_gettime(file->timerfd, &value);
    if (status < 0) return status;
    return copy_to_user(user_value, &value, sizeof(value)) == 0 ? 0 : -EFAULT;
}

static int64_t sys_epoll_create(int flags) {
    if (flags & ~EPOLL_CLOEXEC) return -EINVAL;
    struct epoll_context *context = epoll_create();
    if (!context) return -ENOMEM;
    struct file *file = file_create_epoll(context, 0);
    if (!file) {
        epoll_destroy(context);
        return -ENOMEM;
    }
    return install_new_file(file, flags & EPOLL_CLOEXEC);
}

static int64_t sys_epoll_ctl(int epoll_fd, int operation, int target_fd,
                             uint64_t user_event) {
    struct file *epoll_file = file_from_fd(epoll_fd);
    struct file *target_file = file_from_fd(target_fd);
    if (!epoll_file || epoll_file->kind != FILE_KIND_EPOLL || !target_file)
        return -EBADF;
    if (epoll_file == target_file) return -EINVAL;
    struct tunix_epoll_event event;
    if (operation != EPOLL_CTL_DEL) {
        if (!user_event || copy_from_user(&event, user_event, sizeof(event)) != 0)
            return -EFAULT;
    } else {
        memset(&event, 0, sizeof(event));
    }
    if (operation == EPOLL_CTL_ADD)
        return epoll_ctl_add(epoll_file->epoll, target_fd, target_file, &event);
    if (operation == EPOLL_CTL_MOD)
        return epoll_ctl_mod(epoll_file->epoll, target_fd, target_file, &event);
    if (operation == EPOLL_CTL_DEL)
        return epoll_ctl_del(epoll_file->epoll, target_fd, target_file);
    return -EINVAL;
}

static int64_t sys_epoll_wait_once(int epoll_fd, uint64_t user_events,
                                   int maximum, int commit_empty) {
    if (!user_events || maximum <= 0 || maximum > 128) return -EINVAL;
    struct file *file = file_from_fd(epoll_fd);
    if (!file || file->kind != FILE_KIND_EPOLL) return -EBADF;
    struct tunix_epoll_event events[128];
    int ready = epoll_collect(file->epoll, events, maximum);
    if (ready < 0) return ready;
    if ((ready || commit_empty) && ready > 0 &&
        copy_to_user(user_events, events, (size_t)ready * sizeof(events[0])) != 0)
        return -EFAULT;
    return ready;
}

static int64_t sys_inotify_init(int flags) {
    if (flags & ~(IN_NONBLOCK | IN_CLOEXEC)) return -EINVAL;
    struct inotify_context *context = inotify_create();
    if (!context) return -ENOMEM;
    struct file *file = file_create_inotify(context,
        (uint32_t)(flags & IN_NONBLOCK));
    if (!file) {
        inotify_destroy(context);
        return -ENOMEM;
    }
    return install_new_file(file, flags & IN_CLOEXEC);
}

static int64_t sys_inotify_add_watch(int fd, uint64_t user_path, uint32_t mask) {
    struct file *file = file_from_fd(fd);
    if (!file || file->kind != FILE_KIND_INOTIFY) return -EBADF;
    char path[256];
    if (copy_string_from_user(path, sizeof(path), user_path) < 0) return -EFAULT;
    struct vfs_node *node = vfs_lookup(path);
    if (!node) return -ENOENT;
    return inotify_add_watch(file->inotify, node, mask);
}

static int64_t sys_inotify_rm_watch(int fd, int descriptor) {
    struct file *file = file_from_fd(fd);
    if (!file || file->kind != FILE_KIND_INOTIFY) return -EBADF;
    return inotify_remove_watch(file->inotify, descriptor);
}

void syscall_dispatch(struct syscall_frame *frame) {
    if (!frame) return;
    process_account_runtime();
    process_reap_deferred();
    struct process *caller = process_current();
    uint64_t syscall_number = frame->rax;
    if (caller) caller->syscall_rewound = 0;
    if (caller && caller->io_wait_active && caller->io_wait_syscall != syscall_number)
        clear_io_wait(caller);
    int skip_signal_delivery = 0;

    switch (syscall_number) {
        case SYS_READ: {
            int fd = (int)frame->rdi;
            int64_t result = sys_read(fd, frame->rsi, (size_t)frame->rdx);
            struct process *process = process_current();
            struct file *file = process && fd >= 0 && fd < PROCESS_MAX_FDS ? process->fds[fd] : NULL;
            if (result == -EAGAIN && file && !(file->flags & O_NONBLOCK)) {
                block_and_retry(frame, SYS_READ, file, 0);
            } else {
                frame->rax = (uint64_t)result;
            }
            break;
        }
        case SYS_WRITE: {
            int fd = (int)frame->rdi;
            int64_t result = sys_write(fd, frame->rsi, (size_t)frame->rdx);
            struct file *file = file_from_fd(fd);
            if (result == -EAGAIN && file && !(file->flags & O_NONBLOCK)) {
                block_and_retry(frame, SYS_WRITE, file, 1);
            } else {
                frame->rax = (uint64_t)result;
            }
            break;
        }
        case SYS_OPEN: frame->rax = (uint64_t)open_at(AT_FDCWD, frame->rdi, frame->rsi, frame->rdx); break;
        case SYS_CLOSE: frame->rax = (uint64_t)sys_close((int)frame->rdi); break;
        case SYS_POLL: {
            int timeout_ms = (int)frame->rdx;
            int64_t timeout_ns = timeout_ms_to_ns(timeout_ms);
            int64_t result = sys_poll_once(frame->rdi, frame->rsi, timeout_ms == 0);
            if (result != 0 || timeout_ms == 0) {
                clear_io_wait(process_current());
                frame->rax = (uint64_t)result;
            } else if (!retry_io_wait(frame, SYS_POLL, timeout_ns)) {
                frame->rax = (uint64_t)sys_poll_once(frame->rdi, frame->rsi, 1);
            }
            break;
        }
        case SYS_STAT: frame->rax = (uint64_t)stat_path(AT_FDCWD, frame->rdi, frame->rsi, 1); break;
        case SYS_LSTAT: frame->rax = (uint64_t)stat_path(AT_FDCWD, frame->rdi, frame->rsi, 0); break;
        case SYS_FSTAT: frame->rax = (uint64_t)sys_fstat((int)frame->rdi, frame->rsi); break;
        case SYS_LSEEK: frame->rax = (uint64_t)sys_lseek((int)frame->rdi, (int64_t)frame->rsi, (int)frame->rdx); break;
        case SYS_MMAP: frame->rax = (uint64_t)sys_mmap(frame->rdi, frame->rsi, (int)frame->rdx, (int)frame->r10, (int)frame->r8, frame->r9); break;
        case SYS_MPROTECT: frame->rax = (uint64_t)sys_mprotect(frame->rdi, frame->rsi, (int)frame->rdx); break;
        /* Advisory memory/file hints: our VM eagerly backs every mapping and the
         * page cache is write-through, so there is nothing to prefetch, flush or
         * drop. Returning 0 (rather than ENOSYS) matters because these are public
         * libc wrappers that set errno on failure -- a stale ENOSYS then leaks
         * into later errno checks. madvise/msync/posix_fadvise are all defined as
         * best-effort, so a no-op is a conforming implementation. */
        case SYS_MADVISE: frame->rax = 0; break;
        case SYS_MSYNC: frame->rax = 0; break;
        case SYS_FADVISE64: frame->rax = 0; break;
        case SYS_MUNMAP: frame->rax = (uint64_t)sys_munmap(frame->rdi, frame->rsi); break;
        case SYS_BRK: frame->rax = (uint64_t)sys_brk(frame->rdi); break;
        case SYS_RT_SIGACTION: frame->rax = (uint64_t)sys_sigaction((int)frame->rdi, frame->rsi, frame->rdx, frame->r10); break;
        case SYS_RT_SIGPROCMASK: frame->rax = (uint64_t)sys_sigprocmask((int)frame->rdi, frame->rsi, frame->rdx, frame->r10); break;
        case SYS_RT_SIGRETURN:
            if (process_sigreturn(frame) != 0) frame->rax = (uint64_t)-(int64_t)EINVAL;
            skip_signal_delivery = 1;
            break;
        case SYS_IOCTL: frame->rax = (uint64_t)sys_ioctl((int)frame->rdi, (unsigned long)frame->rsi, frame->rdx); break;
        case SYS_PREAD64: {
            struct process *process = process_current();
            int fd = (int)frame->rdi;
            if (!process || fd < 0 || fd >= PROCESS_MAX_FDS || !process->fds[fd] || process->fds[fd]->kind != FILE_KIND_VFS) frame->rax = (uint64_t)-(int64_t)EBADF;
            else {
                uint64_t saved = process->fds[fd]->offset;
                process->fds[fd]->offset = frame->r10;
                frame->rax = (uint64_t)sys_read(fd, frame->rsi, (size_t)frame->rdx);
                process->fds[fd]->offset = saved;
            }
            break;
        }
        case SYS_PWRITE64: {
            struct process *process = process_current();
            int fd = (int)frame->rdi;
            if (!process || fd < 0 || fd >= PROCESS_MAX_FDS || !process->fds[fd] || process->fds[fd]->kind != FILE_KIND_VFS) frame->rax = (uint64_t)-(int64_t)EBADF;
            else {
                uint64_t saved = process->fds[fd]->offset;
                process->fds[fd]->offset = frame->r10;
                frame->rax = (uint64_t)sys_write(fd, frame->rsi, (size_t)frame->rdx);
                process->fds[fd]->offset = saved;
            }
            break;
        }
        case SYS_READV:
        case SYS_WRITEV: {
            int writing = syscall_number == SYS_WRITEV;
            int fd = (int)frame->rdi;
            int64_t result = sys_readv_writev(fd, frame->rsi, (int)frame->rdx, writing);
            /* readv/writev block exactly like read/write; coreutils reaches a
               pipe through these, so leaving them unblocking surfaced EAGAIN
               to userspace as "Resource temporarily unavailable". */
            struct file *file = file_from_fd(fd);
            if (result == -EAGAIN && file && !(file->flags & O_NONBLOCK))
                block_and_retry(frame, syscall_number, file, writing);
            else
                frame->rax = (uint64_t)result;
            break;
        }
        case SYS_ACCESS: {
            char path[256];
            int status = copy_path_at(AT_FDCWD, frame->rdi, path);
            frame->rax = (uint64_t)(status != 0 ? status : (vfs_lookup(path) ? 0 : -ENOENT));
            break;
        }
        case SYS_PSELECT6: {
            int64_t timeout_ns = read_timespec_timeout_ns(frame->r8);
            if (timeout_ns < -1) {
                clear_io_wait(process_current());
                frame->rax = (uint64_t)timeout_ns;
                break;
            }
            int64_t result = sys_select_once((int)frame->rdi, frame->rsi, frame->rdx,
                                             frame->r10, timeout_ns == 0);
            if (result != 0 || timeout_ns == 0) {
                clear_io_wait(process_current());
                frame->rax = (uint64_t)result;
            } else if (!retry_io_wait(frame, SYS_PSELECT6, timeout_ns)) {
                frame->rax = (uint64_t)sys_select_once((int)frame->rdi, frame->rsi,
                                                       frame->rdx, frame->r10, 1);
            }
            break;
        }
        case SYS_PPOLL: {
            int64_t timeout_ns = read_timespec_timeout_ns(frame->rdx);
            if (timeout_ns < -1) {
                clear_io_wait(process_current());
                frame->rax = (uint64_t)timeout_ns;
                break;
            }
            int64_t result = sys_poll_once(frame->rdi, frame->rsi, timeout_ns == 0);
            if (result != 0 || timeout_ns == 0) {
                clear_io_wait(process_current());
                frame->rax = (uint64_t)result;
            } else if (!retry_io_wait(frame, SYS_PPOLL, timeout_ns)) {
                frame->rax = (uint64_t)sys_poll_once(frame->rdi, frame->rsi, 1);
            }
            break;
        }
        case SYS_FACCESSAT: {
            char path[256];
            int status = copy_path_at((int)frame->rdi, frame->rsi, path);
            frame->rax = (uint64_t)(status != 0 ? status : (vfs_lookup(path) ? 0 : -ENOENT));
            break;
        }
        case SYS_PIPE: frame->rax = (uint64_t)sys_pipe(frame->rdi, 0); break;
        case SYS_SELECT: {
            int64_t timeout_ns = read_timeval_timeout_ns(frame->r8);
            if (timeout_ns < -1) {
                clear_io_wait(process_current());
                frame->rax = (uint64_t)timeout_ns;
                break;
            }
            int64_t result = sys_select_once((int)frame->rdi, frame->rsi, frame->rdx,
                                             frame->r10, timeout_ns == 0);
            if (result != 0 || timeout_ns == 0) {
                clear_io_wait(process_current());
                frame->rax = (uint64_t)result;
            } else if (!retry_io_wait(frame, SYS_SELECT, timeout_ns)) {
                frame->rax = (uint64_t)sys_select_once((int)frame->rdi, frame->rsi,
                                                       frame->rdx, frame->r10, 1);
            }
            break;
        }
        case SYS_SCHED_YIELD: frame->rax = 0; process_yield_from_syscall(frame); break;
        case SYS_EPOLL_CREATE:
            frame->rax = frame->rdi == 0 ? (uint64_t)-(int64_t)EINVAL :
                         (uint64_t)sys_epoll_create(0);
            break;
        case SYS_DUP: frame->rax = (uint64_t)sys_dup((int)frame->rdi, 0, 0); break;
        case SYS_DUP2: frame->rax = (uint64_t)sys_dup_to((int)frame->rdi, (int)frame->rsi, 0, 0); break;
        case SYS_DUP3:
            if (frame->rdx & ~O_CLOEXEC) frame->rax = (uint64_t)-(int64_t)EINVAL;
            else frame->rax = (uint64_t)sys_dup_to((int)frame->rdi, (int)frame->rsi,
                                                   (frame->rdx & O_CLOEXEC) != 0, 1);
            break;
        case SYS_NANOSLEEP: {
            struct linux_timespec request;
            if (copy_from_user(&request, frame->rdi, sizeof(request)) != 0) {
                frame->rax = (uint64_t)-(int64_t)EFAULT;
                break;
            }
            if (request.tv_sec < 0 || request.tv_nsec < 0 || request.tv_nsec >= 1000000000LL) {
                frame->rax = (uint64_t)-(int64_t)EINVAL;
                break;
            }
            int64_t duration = request.tv_sec > (INT64_MAX - request.tv_nsec) / 1000000000LL ?
                INT64_MAX : request.tv_sec * 1000000000LL + request.tv_nsec;
            if (duration == 0) frame->rax = 0;
            else if (!retry_io_wait(frame, SYS_NANOSLEEP, duration)) frame->rax = 0;
            break;
        }
        case SYS_GETITIMER: {
            struct process *process = process_current();
            if ((int)frame->rdi != 0 /* ITIMER_REAL */) {
                frame->rax = (uint64_t)-(int64_t)EINVAL;
                break;
            }
            if (!process) {
                frame->rax = (uint64_t)-(int64_t)EINVAL;
                break;
            }
            uint64_t now = time_uptime_ns();
            uint64_t remaining_ns = process->itimer_real_deadline_ns > now ?
                process->itimer_real_deadline_ns - now : 0;
            struct linux_timeval current_value[2];
            current_value[0].tv_sec = (int64_t)(process->itimer_real_interval_ns / 1000000000ULL);
            current_value[0].tv_usec = (int64_t)((process->itimer_real_interval_ns / 1000ULL) % 1000000ULL);
            current_value[1].tv_sec = (int64_t)(remaining_ns / 1000000000ULL);
            current_value[1].tv_usec = (int64_t)((remaining_ns / 1000ULL) % 1000000ULL);
            if (frame->rsi && copy_to_user(frame->rsi, current_value, sizeof(current_value)) != 0) {
                frame->rax = (uint64_t)-(int64_t)EFAULT;
                break;
            }
            frame->rax = 0;
            break;
        }
        case SYS_SETITIMER: {
            struct process *process = process_current();
            if ((int)frame->rdi != 0 /* ITIMER_REAL */) {
                frame->rax = (uint64_t)-(int64_t)EINVAL;
                break;
            }
            if (!process || !frame->rsi) {
                frame->rax = (uint64_t)-(int64_t)EFAULT;
                break;
            }
            struct linux_timeval new_value[2];
            if (copy_from_user(new_value, frame->rsi, sizeof(new_value)) != 0) {
                frame->rax = (uint64_t)-(int64_t)EFAULT;
                break;
            }
            if (new_value[0].tv_sec < 0 || new_value[0].tv_usec < 0 || new_value[0].tv_usec >= 1000000LL ||
                new_value[1].tv_sec < 0 || new_value[1].tv_usec < 0 || new_value[1].tv_usec >= 1000000LL) {
                frame->rax = (uint64_t)-(int64_t)EINVAL;
                break;
            }
            uint64_t now = time_uptime_ns();
            if (frame->rdx) {
                uint64_t remaining_ns = process->itimer_real_deadline_ns > now ?
                    process->itimer_real_deadline_ns - now : 0;
                struct linux_timeval old_value[2];
                old_value[0].tv_sec = (int64_t)(process->itimer_real_interval_ns / 1000000000ULL);
                old_value[0].tv_usec = (int64_t)((process->itimer_real_interval_ns / 1000ULL) % 1000000ULL);
                old_value[1].tv_sec = (int64_t)(remaining_ns / 1000000000ULL);
                old_value[1].tv_usec = (int64_t)((remaining_ns / 1000ULL) % 1000000ULL);
                if (copy_to_user(frame->rdx, old_value, sizeof(old_value)) != 0) {
                    frame->rax = (uint64_t)-(int64_t)EFAULT;
                    break;
                }
            }
            uint64_t interval_ns = (uint64_t)new_value[0].tv_sec * 1000000000ULL +
                                    (uint64_t)new_value[0].tv_usec * 1000ULL;
            uint64_t value_ns = (uint64_t)new_value[1].tv_sec * 1000000000ULL +
                                (uint64_t)new_value[1].tv_usec * 1000ULL;
            process->itimer_real_interval_ns = interval_ns;
            process->itimer_real_deadline_ns = value_ns ? now + value_ns : 0;
            frame->rax = 0;
            break;
        }
        case SYS_ALARM: {
            struct process *process = process_current();
            if (!process) {
                frame->rax = 0;
                break;
            }
            uint64_t seconds = frame->rdi & 0xFFFFFFFFULL;
            uint64_t now = time_uptime_ns();
            uint64_t remaining_ns = process->itimer_real_deadline_ns > now ?
                process->itimer_real_deadline_ns - now : 0;
            uint64_t remaining_sec = remaining_ns / 1000000000ULL;
            if (remaining_ns % 1000000000ULL) remaining_sec++;
            process->itimer_real_interval_ns = 0;
            process->itimer_real_deadline_ns = seconds ? now + seconds * 1000000000ULL : 0;
            frame->rax = remaining_sec;
            break;
        }
        case SYS_EPOLL_WAIT:
        case SYS_EPOLL_PWAIT: {
            int timeout_ms = (int)frame->r10;
            int64_t timeout_ns = timeout_ms_to_ns(timeout_ms);
            int64_t result = sys_epoll_wait_once((int)frame->rdi, frame->rsi,
                                                  (int)frame->rdx, timeout_ms == 0);
            if (result != 0 || timeout_ms == 0) {
                clear_io_wait(process_current());
                frame->rax = (uint64_t)result;
            } else if (!retry_io_wait(frame, syscall_number, timeout_ns)) {
                frame->rax = (uint64_t)sys_epoll_wait_once((int)frame->rdi, frame->rsi,
                                                            (int)frame->rdx, 1);
            }
            break;
        }
        case SYS_EPOLL_CTL:
            frame->rax = (uint64_t)sys_epoll_ctl((int)frame->rdi, (int)frame->rsi,
                                                 (int)frame->rdx, frame->r10);
            break;
        case SYS_SOCKET: frame->rax = (uint64_t)sys_socket((int)frame->rdi, (int)frame->rsi, (int)frame->rdx); break;
        case SYS_CONNECT: {
            int fd = (int)frame->rdi;
            int64_t result = sys_connect(fd, frame->rsi, frame->rdx);
            struct process *process = process_current();
            struct file *file = process && fd >= 0 && fd < PROCESS_MAX_FDS ? process->fds[fd] : NULL;
            /* A TCP connect() is asynchronous: it returns -EINPROGRESS while the
               handshake is in flight. Block by re-issuing the syscall (each retry
               pumps net_poll) unless the socket is non-blocking. */
            if (result == -EINPROGRESS && file && file->kind == FILE_KIND_INET_SOCKET &&
                !(file->flags & O_NONBLOCK)) {
                frame->user_rip -= 2U;
                frame->rax = SYS_CONNECT;
                if (process) process->syscall_rewound = 1;
                process_yield_from_syscall(frame);
            } else {
                frame->rax = (uint64_t)result;
            }
            break;
        }
        case SYS_ACCEPT: frame->rax = (uint64_t)sys_accept((int)frame->rdi, frame->rsi, frame->rdx, 0); break;
        case SYS_SENDTO: frame->rax = (uint64_t)sys_sendto((int)frame->rdi, frame->rsi, frame->rdx, (int)frame->r10, frame->r8, frame->r9); break;
        case SYS_RECVFROM: {
            int fd = (int)frame->rdi;
            int flags = (int)frame->r10;
            int64_t result = sys_recvfrom(fd, frame->rsi, frame->rdx, flags, frame->r8, frame->r9);
            struct process *process = process_current();
            struct file *file = process && fd >= 0 && fd < PROCESS_MAX_FDS ? process->fds[fd] : NULL;
            if (result == -EAGAIN && file && !(file->flags & O_NONBLOCK) && !(flags & MSG_DONTWAIT)) {
                frame->user_rip -= 2U;
                frame->rax = SYS_RECVFROM;
                if (process) process->syscall_rewound = 1;
                process_yield_from_syscall(frame);
            } else {
                frame->rax = (uint64_t)result;
            }
            break;
        }
        case SYS_SENDMSG: frame->rax = (uint64_t)sys_sendmsg((int)frame->rdi, frame->rsi, (int)frame->rdx); break;
        case SYS_RECVMSG: {
            int fd = (int)frame->rdi;
            int flags = (int)frame->rdx;
            int64_t result = sys_recvmsg(fd, frame->rsi, flags);
            struct process *process = process_current();
            struct file *file = process && fd >= 0 && fd < PROCESS_MAX_FDS ? process->fds[fd] : NULL;
            if (result == -EAGAIN && file && !(file->flags & O_NONBLOCK) && !(flags & MSG_DONTWAIT)) {
                frame->user_rip -= 2U;
                frame->rax = SYS_RECVMSG;
                if (process) process->syscall_rewound = 1;
                process_yield_from_syscall(frame);
            } else {
                frame->rax = (uint64_t)result;
            }
            break;
        }
        case SYS_SHUTDOWN: frame->rax = (uint64_t)sys_shutdown((int)frame->rdi, (int)frame->rsi); break;
        case SYS_BIND: frame->rax = (uint64_t)sys_bind((int)frame->rdi, frame->rsi, frame->rdx); break;
        case SYS_LISTEN: frame->rax = (uint64_t)sys_listen((int)frame->rdi, (int)frame->rsi); break;
        case SYS_GETSOCKNAME: frame->rax = (uint64_t)sys_socket_name((int)frame->rdi, frame->rsi, frame->rdx, 0); break;
        case SYS_GETPEERNAME: frame->rax = (uint64_t)sys_socket_name((int)frame->rdi, frame->rsi, frame->rdx, 1); break;
        case SYS_SOCKETPAIR: frame->rax = (uint64_t)sys_socketpair((int)frame->rdi, (int)frame->rsi, (int)frame->rdx, frame->r10); break;
        case SYS_SETSOCKOPT: frame->rax = (uint64_t)sys_setsockopt((int)frame->rdi, (int)frame->rsi, (int)frame->rdx, frame->r10, frame->r8); break;
        case SYS_GETSOCKOPT: frame->rax = (uint64_t)sys_getsockopt((int)frame->rdi, (int)frame->rsi, (int)frame->rdx, frame->r10, frame->r8); break;
        case SYS_GETPID: frame->rax = process_current_pid(); break;
        case SYS_GETTID: frame->rax = process_current_tid(); break;
        case SYS_CLONE: {
            int64_t pid = sys_clone_fork_compat(
                frame, frame->rdi, frame->rsi, frame->rdx, frame->r10, frame->r8);
            frame->rax = (uint64_t)pid;
            if (pid > 0) process_run_child_first_from_syscall(frame, (uint64_t)pid);
            break;
        }
        case SYS_CLONE3: {
            int64_t pid = sys_clone3_fork_compat(frame, frame->rdi, (size_t)frame->rsi);
            frame->rax = (uint64_t)pid;
            if (pid > 0) process_run_child_first_from_syscall(frame, (uint64_t)pid);
            break;
        }
        case SYS_FORK:
        case SYS_VFORK: {
            int64_t pid = process_fork_from_syscall(frame);
            frame->rax = (uint64_t)pid;
            if (pid > 0) process_run_child_first_from_syscall(frame, (uint64_t)pid);
            break;
        }
        case SYS_EXECVE: frame->rax = (uint64_t)sys_execve(frame, frame->rdi, frame->rsi, frame->rdx); break;
        case SYS_EXIT: process_exit_from_syscall(frame, (int)frame->rdi); break;
        case SYS_EXIT_GROUP: process_exit_group_from_syscall(frame, (int)frame->rdi); break;
        case SYS_WAIT4: {
            int64_t result = process_waitpid_from_syscall(frame, (int64_t)frame->rdi, frame->rsi, (int)frame->rdx);
            if (process_current() == caller) frame->rax = (uint64_t)result;
            break;
        }
        case SYS_KILL: frame->rax = (uint64_t)process_send_signal((int64_t)frame->rdi, (int)frame->rsi); break;
        /* Single-threaded here: a tid is a pid, so tkill(tid,sig) is kill(pid,sig).
         * musl's raise()/abort() route through tkill. */
        case SYS_TKILL: frame->rax = (uint64_t)process_send_signal((int64_t)frame->rdi, (int)frame->rsi); break;
        case SYS_TGKILL: frame->rax = (uint64_t)process_send_signal((int64_t)frame->rsi, (int)frame->rdx); break;
        case SYS_UNAME: frame->rax = (uint64_t)sys_uname(frame->rdi); break;
        case SYS_FCNTL: {
            struct process *process = process_current();
            int fd = (int)frame->rdi;
            int command = (int)frame->rsi;
            if (!process || fd < 0 || fd >= PROCESS_MAX_FDS || !process->fds[fd]) {
                frame->rax = (uint64_t)-(int64_t)EBADF;
            } else if (command == F_DUPFD || command == F_DUPFD_CLOEXEC) {
                frame->rax = (uint64_t)sys_dup(fd, (int)frame->rdx,
                    command == F_DUPFD_CLOEXEC);
            } else if (command == F_GETFD) {
                frame->rax = (process->fd_flags[fd] & PROCESS_FD_CLOEXEC) ? FD_CLOEXEC : 0;
            } else if (command == F_SETFD) {
                if (frame->rdx & ~(uint64_t)FD_CLOEXEC)
                    frame->rax = (uint64_t)-(int64_t)EINVAL;
                else {
                    process->fd_flags[fd] = (frame->rdx & FD_CLOEXEC) ? PROCESS_FD_CLOEXEC : 0;
                    frame->rax = 0;
                }
            } else if (command == F_GETFL) {
                frame->rax = process->fds[fd]->flags;
            } else if (command == F_SETFL) {
                process->fds[fd]->flags =
                    (process->fds[fd]->flags & ~(uint32_t)O_NONBLOCK) |
                    ((uint32_t)frame->rdx & (uint32_t)O_NONBLOCK);
                frame->rax = 0;
            } else {
                frame->rax = (uint64_t)-(int64_t)EINVAL;
            }
            break;
        }
        case SYS_FSYNC: frame->rax = (uint64_t)sys_fsync((int)frame->rdi); break;
        case SYS_FDATASYNC: frame->rax = (uint64_t)sys_fsync((int)frame->rdi); break;
        case SYS_SYNCFS: frame->rax = (uint64_t)sys_fsync((int)frame->rdi); break;
        case SYS_SYNC: frame->rax = (uint64_t)ext2fs_sync(); break;
        case SYS_FTRUNCATE: frame->rax = (uint64_t)sys_ftruncate((int)frame->rdi, frame->rsi); break;
        case SYS_GETCWD: frame->rax = (uint64_t)sys_getcwd(frame->rdi, (size_t)frame->rsi); break;
        case SYS_CHDIR: frame->rax = (uint64_t)sys_chdir(frame->rdi); break;
        case SYS_FCHDIR: frame->rax = (uint64_t)sys_fchdir((int)frame->rdi); break;
        case SYS_RENAME: frame->rax = (uint64_t)sys_rename_at(AT_FDCWD, frame->rdi, AT_FDCWD, frame->rsi, 0); break;
        case SYS_MKDIR: frame->rax = (uint64_t)sys_mkdir_at(AT_FDCWD, frame->rdi, frame->rsi); break;
        case SYS_RMDIR: frame->rax = (uint64_t)sys_unlink_at(AT_FDCWD, frame->rdi, AT_REMOVEDIR); break;
        case SYS_UNLINK: frame->rax = (uint64_t)sys_unlink_at(AT_FDCWD, frame->rdi, 0); break;
        case SYS_READLINK: frame->rax = (uint64_t)sys_readlink_at(AT_FDCWD, frame->rdi, frame->rsi, (size_t)frame->rdx); break;
        case SYS_CHMOD: frame->rax = (uint64_t)sys_chmod_at(AT_FDCWD, frame->rdi, (uint32_t)frame->rsi, 0); break;
        case SYS_FCHMOD: frame->rax = (uint64_t)sys_fchmod((int)frame->rdi, (uint32_t)frame->rsi); break;
        case SYS_UMASK: frame->rax = process_set_umask((uint32_t)frame->rdi); break;
        case SYS_GETTIMEOFDAY: frame->rax = (uint64_t)sys_gettimeofday(frame->rdi); break;
        case SYS_GETRLIMIT: frame->rax = (uint64_t)sys_prlimit(frame->rsi); break;
        case SYS_GETRUSAGE: {
            uint8_t zero[144]; memset(zero, 0, sizeof(zero));
            frame->rax = copy_to_user(frame->rsi, zero, sizeof(zero)) == 0 ? 0 : (uint64_t)-(int64_t)EFAULT;
            break;
        }
        case SYS_GETUID:
        case SYS_GETGID:
        case SYS_GETEUID:
        case SYS_GETEGID: frame->rax = 0; break;
        case SYS_GETGROUPS:
            if ((int64_t)frame->rdi < 0) frame->rax = (uint64_t)-(int64_t)EINVAL;
            else frame->rax = 0;
            break;
        case SYS_SETPGID: frame->rax = (uint64_t)process_setpgid((int64_t)frame->rdi, (int64_t)frame->rsi); break;
        case SYS_GETPPID: frame->rax = process_current_ppid(); break;
        case SYS_GETPGRP: frame->rax = process_current() ? process_current()->pgid : 0; break;
        case SYS_SETSID: frame->rax = (uint64_t)process_setsid(); break;
        case SYS_GETPGID: {
            struct process *target = frame->rdi ? process_find(frame->rdi) : process_current();
            frame->rax = target ? target->pgid : (uint64_t)-(int64_t)ESRCH;
            break;
        }
        case SYS_GETSID: {
            struct process *target = frame->rdi ? process_find(frame->rdi) : process_current();
            frame->rax = target ? target->sid : (uint64_t)-(int64_t)ESRCH;
            break;
        }
        case SYS_SIGALTSTACK: frame->rax = (uint64_t)sys_sigaltstack(frame, frame->rdi, frame->rsi); break;
        case SYS_PRCTL: frame->rax = (uint64_t)sys_prctl((int)frame->rdi, frame->rsi, frame->rdx, frame->r10, frame->r8); break;
        case SYS_ARCH_PRCTL: frame->rax = (uint64_t)sys_arch_prctl((int)frame->rdi, frame->rsi); break;
        case SYS_FUTEX: {
            int operation = (int)frame->rsi;
            int command = operation & FUTEX_CMD_MASK;
            if (command == FUTEX_WAKE) {
                frame->rax = (uint64_t)process_futex_wake(frame->rdi, (int)frame->rdx);
            } else if (command == FUTEX_WAIT) {
                int64_t timeout_ns = -1;
                if (frame->r10) {
                    struct linux_timespec timeout;
                    if (copy_from_user(&timeout, frame->r10, sizeof(timeout)) != 0) {
                        frame->rax = (uint64_t)-(int64_t)EFAULT;
                        break;
                    }
                    if (timeout.tv_sec < 0 || timeout.tv_nsec < 0 || timeout.tv_nsec >= 1000000000LL) {
                        frame->rax = (uint64_t)-(int64_t)EINVAL;
                        break;
                    }
                    timeout_ns = timeout.tv_sec > (INT64_MAX - timeout.tv_nsec) / 1000000000LL ?
                        INT64_MAX : timeout.tv_sec * 1000000000LL + timeout.tv_nsec;
                }
                struct process *futex_caller = process_current();
                int64_t result = process_futex_wait(frame, frame->rdi, (uint32_t)frame->rdx,
                                                    timeout_ns);
                if (process_current() == futex_caller) frame->rax = (uint64_t)result;
            } else {
                frame->rax = (uint64_t)-(int64_t)ENOSYS;
            }
            break;
        }
        case SYS_SET_TID_ADDRESS: {
            struct process *process = process_current();
            if (process) process->clear_child_tid_user = frame->rdi;
            frame->rax = process_current_pid();
            break;
        }
        case SYS_CLOCK_GETTIME: frame->rax = (uint64_t)sys_clock_gettime((int)frame->rdi, frame->rsi); break;
        case SYS_CLOCK_GETRES: {
            struct linux_timespec value = {0, 1000000};
            frame->rax = frame->rsi && copy_to_user(frame->rsi, &value, sizeof(value)) != 0 ? (uint64_t)-(int64_t)EFAULT : 0;
            break;
        }
        case SYS_CLOCK_NANOSLEEP: {
            if ((int)frame->rdi != 0 && (int)frame->rdi != 1 && (int)frame->rdi != 7) {
                frame->rax = (uint64_t)-(int64_t)EINVAL;
                break;
            }
            if (frame->rsi & ~1ULL) {
                frame->rax = (uint64_t)-(int64_t)EINVAL;
                break;
            }
            struct linux_timespec request;
            if (copy_from_user(&request, frame->rdx, sizeof(request)) != 0 ||
                request.tv_sec < 0 || request.tv_nsec < 0 ||
                request.tv_nsec >= 1000000000LL) {
                frame->rax = (uint64_t)-(int64_t)EINVAL;
                break;
            }
            uint64_t requested = (uint64_t)request.tv_sec * 1000000000ULL +
                                 (uint64_t)request.tv_nsec;
            int64_t duration;
            if (frame->rsi & 1ULL) {
                uint64_t now = (int)frame->rdi == 0 ? time_realtime_ns() : time_uptime_ns();
                duration = requested <= now ? 0 :
                    (requested - now > (uint64_t)INT64_MAX ? INT64_MAX :
                     (int64_t)(requested - now));
            } else {
                duration = requested > (uint64_t)INT64_MAX ? INT64_MAX : (int64_t)requested;
            }
            if (duration == 0) frame->rax = 0;
            else if (!retry_io_wait(frame, SYS_CLOCK_NANOSLEEP, duration)) frame->rax = 0;
            break;
        }
        case SYS_INOTIFY_INIT: frame->rax = (uint64_t)sys_inotify_init(0); break;
        case SYS_INOTIFY_ADD_WATCH:
            frame->rax = (uint64_t)sys_inotify_add_watch((int)frame->rdi, frame->rsi,
                                                         (uint32_t)frame->rdx);
            break;
        case SYS_INOTIFY_RM_WATCH:
            frame->rax = (uint64_t)sys_inotify_rm_watch((int)frame->rdi, (int)frame->rsi);
            break;
        case SYS_TIMERFD_CREATE:
            frame->rax = (uint64_t)sys_timerfd_create((int)frame->rdi, (int)frame->rsi);
            break;
        case SYS_EVENTFD:
            frame->rax = (uint64_t)sys_eventfd((uint32_t)frame->rdi, 0, 1);
            break;
        case SYS_TIMERFD_SETTIME:
            frame->rax = (uint64_t)sys_timerfd_settime((int)frame->rdi,
                (int)frame->rsi, frame->rdx, frame->r10);
            break;
        case SYS_TIMERFD_GETTIME:
            frame->rax = (uint64_t)sys_timerfd_gettime((int)frame->rdi, frame->rsi);
            break;
        case SYS_EVENTFD2:
            frame->rax = (uint64_t)sys_eventfd((uint32_t)frame->rdi,
                                               (int)frame->rsi, 0);
            break;
        case SYS_EPOLL_CREATE1:
            frame->rax = (uint64_t)sys_epoll_create((int)frame->rdi);
            break;
        case SYS_INOTIFY_INIT1:
            frame->rax = (uint64_t)sys_inotify_init((int)frame->rdi);
            break;
        case SYS_OPENAT: frame->rax = (uint64_t)open_at((int)frame->rdi, frame->rsi, frame->rdx, frame->r10); break;
        case SYS_MKDIRAT: frame->rax = (uint64_t)sys_mkdir_at((int)frame->rdi, frame->rsi, frame->rdx); break;
        case SYS_NEWFSTATAT:
            if (frame->r10 & ~(AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH | AT_NO_AUTOMOUNT))
                frame->rax = (uint64_t)-(int64_t)EINVAL;
            else {
                char first = 0;
                if (copy_from_user(&first, frame->rsi, 1) != 0) frame->rax = (uint64_t)-(int64_t)EFAULT;
                else if (!first && (frame->r10 & AT_EMPTY_PATH) && (int)frame->rdi >= 0)
                    frame->rax = (uint64_t)sys_fstat((int)frame->rdi, frame->rdx);
                else frame->rax = (uint64_t)stat_path((int)frame->rdi, frame->rsi, frame->rdx,
                                                      (frame->r10 & AT_SYMLINK_NOFOLLOW) == 0);
            }
            break;
        case SYS_UNLINKAT: frame->rax = (uint64_t)sys_unlink_at((int)frame->rdi, frame->rsi, (int)frame->rdx); break;
        case SYS_RENAMEAT: frame->rax = (uint64_t)sys_rename_at((int)frame->rdi, frame->rsi, (int)frame->rdx, frame->r10, 0); break;
        case SYS_SYMLINK: frame->rax = (uint64_t)sys_symlink_at(frame->rdi, AT_FDCWD, frame->rsi); break;
        case SYS_SYMLINKAT: frame->rax = (uint64_t)sys_symlink_at(frame->rdi, (int)frame->rsi, frame->rdx); break;
        case SYS_READLINKAT: frame->rax = (uint64_t)sys_readlink_at((int)frame->rdi, frame->rsi, frame->rdx, (size_t)frame->r10); break;
        case SYS_FCHMODAT: frame->rax = (uint64_t)sys_chmod_at((int)frame->rdi, frame->rsi, (uint32_t)frame->rdx, 0); break;
        case SYS_UTIMENSAT:
            frame->rax = (uint64_t)sys_utimens_at((int)frame->rdi, frame->rsi,
                                                  frame->rdx, (int)frame->r10);
            break;
        case SYS_SET_ROBUST_LIST: frame->rax = (uint64_t)sys_set_robust_list(frame->rdi, (size_t)frame->rsi); break;
        case SYS_GET_ROBUST_LIST: frame->rax = (uint64_t)sys_get_robust_list((int)frame->rdi, frame->rsi, frame->rdx); break;
        case SYS_ACCEPT4: frame->rax = (uint64_t)sys_accept((int)frame->rdi, frame->rsi, frame->rdx, (int)frame->r10); break;
        case SYS_PIPE2: frame->rax = (uint64_t)sys_pipe(frame->rdi, (int)frame->rsi); break;
        case SYS_PRLIMIT64: frame->rax = (uint64_t)sys_prlimit(frame->r10); break;
        case SYS_RENAMEAT2: frame->rax = (uint64_t)sys_rename_at((int)frame->rdi, frame->rsi, (int)frame->rdx, frame->r10, (unsigned)frame->r8); break;
        case SYS_GETRANDOM: frame->rax = (uint64_t)sys_getrandom(frame->rdi, (size_t)frame->rsi, (unsigned)frame->rdx); break;
        case SYS_GETDENTS64: frame->rax = (uint64_t)sys_getdents64((int)frame->rdi, frame->rsi, (size_t)frame->rdx); break;
        case SYS_STATFS: frame->rax = (uint64_t)sys_statfs(frame->rdi, frame->rsi); break;
        case SYS_FSTATFS: frame->rax = (uint64_t)sys_fstatfs((int)frame->rdi, frame->rsi); break;
        case SYS_STATX:
            frame->rax = (uint64_t)sys_statx((int)frame->rdi, frame->rsi, (int)frame->rdx,
                                             (uint32_t)frame->r10, frame->r8);
            break;
        case SYS_RSEQ: frame->rax = (uint64_t)-(int64_t)ENOSYS; break;
        case SYS_FACCESSAT2: {
            unsigned flags = (unsigned)frame->r10;
            if (flags & ~(AT_EACCESS | AT_SYMLINK_NOFOLLOW)) {
                frame->rax = (uint64_t)-(int64_t)EINVAL;
            } else {
                char path[256];
                int status = copy_path_at((int)frame->rdi, frame->rsi, path);
                if (status != 0) {
                    frame->rax = (uint64_t)status;
                } else {
                    struct vfs_node *node = (flags & AT_SYMLINK_NOFOLLOW)
                        ? vfs_lookup_nofollow(path)
                        : vfs_lookup(path);
                    frame->rax = (uint64_t)(node ? 0 : -(int64_t)ENOENT);
                }
            }
            break;
        }
        case SYS_CLOSE_RANGE: {
            struct process *process = process_current();
            uint64_t first = frame->rdi, last = frame->rsi;
            if (!process || first >= PROCESS_MAX_FDS) frame->rax = 0;
            else {
                if (last >= PROCESS_MAX_FDS) last = PROCESS_MAX_FDS - 1;
                for (uint64_t fd = first; fd <= last; fd++) if (process->fds[fd]) process_close_fd(process, (int)fd);
                frame->rax = 0;
            }
            break;
        }
        default:
            /* Gated behind TUNIX_DEBUG_LOGS so an unimplemented syscall never
             * bleeds onto the user's terminal in normal use. kprintf has no
             * length modifiers (no %llu); syscall numbers are small, so %u. */
            KDEBUG("syscall: ENOSYS pid=%u nr=%u\n",
                    (unsigned)process_current_pid(), (unsigned)syscall_number);
            frame->rax = (uint64_t)-(int64_t)ENOSYS;
            break;
    }

    if (!skip_signal_delivery) process_prepare_user_return(frame);
}
