#include <stddef.h>
#include <stdint.h>
#include "include/cred.h"
#include "include/file.h"
#include "include/eventfd.h"
#include "include/timerfd.h"
#include "include/epoll.h"
#include "include/inotify.h"
#include "include/memfd.h"
#include "include/signalfd.h"
#include "include/sysvshm.h"
#include "include/framebuffer.h"
#include "include/input.h"
#include "include/heap.h"
#include "include/klock.h"
#include "include/percpu.h"
#include "include/smp.h"
#include "include/kstring.h"
#include "include/pipe.h"
#include "include/pty.h"
#include "include/pmm.h"
#include "include/power.h"
#include "include/process.h"
#include "include/random.h"
#include "include/signal.h"
#include "include/syscall.h"
#include "include/ext2.h"
#include "include/time.h"
#include "include/tty.h"
#include "include/uts.h"
#include "include/vt.h"
#include "include/usercopy.h"
#include "include/vfs.h"
#include "include/terminal.h"
#include "include/vmm.h"
#include "include/unix_socket.h"
#include "include/net/inet_socket.h"
#include "include/drm.h"
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
#define SYS_MREMAP 25
#define SYS_MUNMAP 11
#define SYS_BRK 12
#define SYS_MSYNC 26
#define SYS_MADVISE 28
#define SYS_SHMGET 29
#define SYS_SHMAT 30
#define SYS_SHMCTL 31
#define SYS_SHMDT 67
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
#define SYS_SCHED_SETSCHEDULER 144
#define SYS_SCHED_GETSCHEDULER 145
#define SYS_SCHED_GETPARAM 143
#define SYS_SCHED_SETPARAM 142
#define SYS_SCHED_GET_PRIORITY_MAX 146
#define SYS_SCHED_GET_PRIORITY_MIN 147
#define SYS_SCHED_RR_GET_INTERVAL 148
#define SYS_GETCPU 309
#define SYS_MEMBARRIER 324
#define SYS_PREADV 295
#define SYS_PWRITEV 296
#define SYS_SCHED_SETAFFINITY 203
#define SYS_SCHED_GETAFFINITY 204
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
#define SYS_SENDMMSG 307
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
#define SYS_CHOWN 92
#define SYS_FCHOWN 93
#define SYS_LCHOWN 94
#define SYS_WAITID 247
#define SYS_FCHOWNAT 260
#define SYS_UNAME 63
#define SYS_TIME 201
#define SYS_SYSINFO 99
#define SYS_TIMES 100
#define SYS_SETHOSTNAME 170
#define SYS_SETDOMAINNAME 171
#define SYS_FCNTL 72
#define SYS_FLOCK 73
#define SYS_FSYNC 74
#define SYS_FDATASYNC 75
#define SYS_STATFS 137
#define SYS_FSTATFS 138
#define SYS_SYNC 162
#define SYS_REBOOT 169
#define SYS_SYNCFS 306
#define SYS_FTRUNCATE 77
#define SYS_GETCWD 79
#define SYS_CHDIR 80
#define SYS_FCHDIR 81
#define SYS_RENAME 82
#define SYS_MKDIR 83
#define SYS_RMDIR 84
#define SYS_MOUNT 165
#define SYS_UMOUNT2 166
#define SYS_LINK 86
#define SYS_UNLINK 87
#define SYS_SYMLINK 88
#define SYS_READLINK 89
#define SYS_CHMOD 90
#define SYS_FCHMOD 91
#define SYS_UMASK 95
#define SYS_GETTIMEOFDAY 96
#define SYS_GETRLIMIT 97
#define SYS_GETRUSAGE 98
#define SYS_GETPRIORITY 140
#define SYS_SETPRIORITY 141
#define SYS_GETUID 102
#define SYS_SETUID 105
#define SYS_SETGID 106
#define SYS_GETGID 104
#define SYS_GETEUID 107
#define SYS_SETREUID 113
#define SYS_SETREGID 114
#define SYS_SETRESUID 117
#define SYS_SETRESGID 119
#define SYS_FALLOCATE 285
#define SYS_GETEGID 108
#define SYS_GETGROUPS 115
#define SYS_SETGROUPS 116
#define SYS_GETRESUID 118
#define SYS_GETRESGID 120
#define SYS_SETFSUID 122
#define SYS_SETFSGID 123
#define SYS_SETPGID 109
#define SYS_GETPPID 110
#define SYS_GETPGRP 111
#define SYS_SETSID 112
#define SYS_GETPGID 121
#define SYS_GETSID 124
#define SYS_CAPGET 125
#define SYS_CAPSET 126
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
#define SYS_MKNOD 133
#define SYS_MKNODAT 259
#define SYS_MKDIRAT 258
#define SYS_NEWFSTATAT 262
#define SYS_UNLINKAT 263
#define SYS_RENAMEAT 264
#define SYS_LINKAT 265
#define SYS_SYMLINKAT 266
#define SYS_READLINKAT 267
#define SYS_FCHMODAT 268
#define SYS_FACCESSAT 269
#define SYS_PSELECT6 270
#define SYS_PPOLL 271
#define SYS_UTIMENSAT 280
#define SYS_EPOLL_PWAIT 281
#define SYS_SIGNALFD 282
#define SYS_TIMERFD_CREATE 283
#define SYS_SIGNALFD4 289
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
#define SYS_MEMFD_CREATE 319
#define SYS_STATX 332
#define SYS_RSEQ 334
#define SYS_CLONE3 435
#define SYS_CLOSE_RANGE 436
#define CLOSE_RANGE_CLOEXEC (1U << 2)
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
#define SFD_NONBLOCK O_NONBLOCK
#define SFD_CLOEXEC O_CLOEXEC
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
#define MFD_CLOEXEC 0x0001U
#define MFD_ALLOW_SEALING 0x0002U
#define SIOCGIFCONF 0x8912U

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

#define PROT_WRITE 0x2
#define PROT_EXEC 0x4
#define MAP_SHARED 0x01
#define MAP_PRIVATE 0x02
#define MAP_FIXED 0x10
#define MREMAP_MAYMOVE 1
#define MAP_ANONYMOUS 0x20
#define MAP_NORESERVE 0x4000
/* Ceiling on what a read-only file mapping shares rather than copies. */
#define SHARED_MAP_MAX_BYTES (256ULL * 1024 * 1024)
#define MAP_FIXED_NOREPLACE 0x100000

#define F_DUPFD 0
#define F_GETFD 1
#define F_SETFD 2
#define F_GETFL 3
#define F_SETFL 4
#define F_ADD_SEALS 1033
#define F_GET_SEALS 1034
#define FIONBIO 0x5421UL
#define F_GETLK 5
#define F_SETLK 6
#define F_SETLKW 7
#define F_RDLCK 0
#define F_WRLCK 1
#define F_UNLCK 2

/* struct flock on x86_64. */
struct linux_flock {
    int16_t type;
    int16_t whence;
    int64_t start;
    int64_t length;
    int32_t pid;
};
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
#define EFBIG 27
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
/* The same two waits and wakes, but the waiter names a set of bits and a wake
   only reaches the waiters it names. glibc issues every timed wait as one of
   these -- and, since 2.31, every untimed one too -- so a kernel without them
   makes any pthread condition variable abort with "the futex facility returned
   an unexpected error code". The other difference is the timeout: a bitset
   wait is given a deadline rather than a duration. */
#define FUTEX_WAIT_BITSET 9
#define FUTEX_WAKE_BITSET 10
#define FUTEX_PRIVATE_FLAG 128
/* Measure the deadline against the wall clock rather than against uptime. */
#define FUTEX_CLOCK_REALTIME 256
#define FUTEX_CMD_MASK 0x7F

#define MAX_EXEC_ITEMS 64
/* 256 was enough for every argv/envp string this kernel had ever been asked
 * to copy -- until a real distribution's login shell exported LS_COLORS.
 * dircolors's default database renders to two or three KiB, so any real
 * userland's first login-shell profile silently broke every execve() after
 * it: copy_string_from_user() has no way to say "didn't fit" separately from
 * "faulted" (see usercopy.c), and copy_exec_vector() turned that into EFAULT
 * for a perfectly valid string. 4096 covers a 256-color LS_COLORS with room
 * to spare.
 *
 * This bound applies only to storage that lives in the kmalloc'd
 * exec_arguments; see MAX_SHEBANG_LINE for the one that does not. Linux draws
 * the same line, for the same reason, and calls the two MAX_ARG_STRLEN and
 * BINPRM_BUF_SIZE. */
#define MAX_EXEC_STRING 4096
/*
 * The "#!" line, which is read into ordinary local buffers rather than onto
 * the heap -- so unlike the limit above, this one is spent out of the 32 KiB
 * kernel stack, and three buffers of it are live at once while a script is
 * being resolved.
 *
 * It was briefly MAX_EXEC_STRING, which put 12 KiB of the exec path's frames
 * on that stack and overran it. A kernel stack is itself a kmalloc'd block
 * (see process.c), so the overrun landed in the neighbouring block's header
 * and did nothing at all until some later kmalloc walked the free list into
 * the wreckage and faulted -- a crash arbitrarily far from the code that
 * caused it. 256 is what Linux allows a shebang line, and no interpreter path
 * worth running is longer.
 */
#define MAX_SHEBANG_LINE 256

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

/* sendmmsg's array element: a message, and room for how much of it went. */
struct linux_mmsghdr {
    struct linux_msghdr msg_hdr;
    uint32_t msg_len;
    uint32_t __pad;
};

_Static_assert(sizeof(struct linux_mmsghdr) == 64, "Linux x86_64 mmsghdr ABI mismatch");

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

/*
 * How much of a write(2) is staged in one go.
 *
 * Every call into file_write() is one filesystem write-back, and on ext2 that
 * means the superblock, the group descriptors and the bitmaps go to the disk
 * with it. Staging a 4 KiB chunk at a time therefore turned a 64 KiB write into
 * sixteen of those, and the metadata dominated: it measured 82% of the cost of
 * writing a file. One block write to the disk is ~0.3 ms whatever it holds, so
 * what matters is how many there are, not how big they are.
 *
 * It matches ext2's run length (EXT2_RUN_BLOCKS * 4 KiB), which is as much as
 * the filesystem will hand the disk in a single command anyway. The buffer
 * comes from the heap because the kernel stack is 16 KiB.
 */
#define WRITE_STAGE_MAX (128U * 1024U)

/* Only regular files get the large staging buffer. A pipe or a socket write is
   allowed to come up short -- that is how O_NONBLOCK reports back pressure --
   and the loop below stops when it does, so a bigger chunk there would turn one
   full write into a short one. Nothing about those paths is slow anyway. */
static int write_stages_large(const struct file *file) {
    return file && file->kind == FILE_KIND_VFS && file->node &&
           (file->node->flags & 0xFFU) == VFS_FILE;
}

static int64_t sys_write(int fd, uint64_t user_buffer, size_t length) {
    struct process *process = process_current();
    if (!process || fd < 0 || fd >= PROCESS_MAX_FDS || !process->files->fds[fd]) return -EBADF;
    struct file *file = process->files->fds[fd];
    uint8_t stage[4096];
    uint8_t *buffer = stage;
    size_t buffer_size = sizeof(stage);
    if (length > sizeof(stage) && write_stages_large(file)) {
        size_t wanted = length < WRITE_STAGE_MAX ? length : WRITE_STAGE_MAX;
        uint8_t *large = (uint8_t *)kmalloc(wanted);
        if (large) {
            buffer = large;
            buffer_size = wanted;
        }
    }

    size_t completed = 0;
    int64_t failure = 0;
    /*
     * An empty write is not always nothing to do. On a message socket it sends
     * an empty datagram, which the reader is meant to see: udevd's workers
     * report that they have finished with exactly that -- a zero-byte message
     * whose whole content is the sender's credentials -- so skipping the call
     * left every device event marked as still running, and every boot waiting
     * out `udevadm settle`. Everything else treats a zero-length write as the
     * no-op it is.
     */
    if (!length) {
        int64_t written = file_write(file, 0, buffer);
        if (written < 0) return written;
        return 0;
    }
    while (completed < length) {
        size_t chunk = length - completed;
        if (chunk > buffer_size) chunk = buffer_size;
        if (copy_from_user(buffer, user_buffer + completed, chunk) != 0) {
            failure = -EFAULT;
            break;
        }
        int64_t written = file_write(file, chunk, buffer);
        if (written == -EPIPE) {
            (void)process_send_signal((int64_t)process->pid, SIGPIPE);
            failure = -EPIPE;
            break;
        }
        if (written < 0) {
            failure = written;
            break;
        }
        completed += (size_t)written;
        if ((size_t)written < chunk) break;
    }

    if (buffer != stage) kfree(buffer);
    if (!completed && failure) return failure;
    return (int64_t)completed;
}

static int64_t sys_read(int fd, uint64_t user_buffer, size_t length) {
    struct process *process = process_current();
    if (!process || fd < 0 || fd >= PROCESS_MAX_FDS || !process->files->fds[fd]) return -EBADF;
    uint8_t buffer[4096];
    size_t completed = 0;
    while (completed < length) {
        size_t chunk = length - completed;
        if (chunk > sizeof(buffer)) chunk = sizeof(buffer);
        int64_t amount = file_read(process->files->fds[fd], chunk, buffer);
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
    /* Asleep rather than round the run queue again: this is poll(), select()
       and the timed waits, and what they are waiting for cannot become true
       while this processor is the one asking. The deadline above is re-tested
       on every wake, so a timeout is still honoured to within a tick. */
    if (process_sleep_on(frame, process_io_wait_channel()) != 0)
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
        if (!process || fd >= PROCESS_MAX_FDS || !process->files->fds[fd]) {
            fds[i].revents = POLLNVAL;
            ready++;
            continue;
        }
        struct file *file = process->files->fds[fd];
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
        if (!process || !process->files->fds[fd]) return -EBADF;
        int this_ready = 0;
        if (fd_set_test(&requested_read, fd) && file_read_ready(process->files->fds[fd]) > 0) {
            fd_set_put(&result_read, fd);
            this_ready = 1;
        }
        if (fd_set_test(&requested_write, fd) && file_write_ready(process->files->fds[fd]) > 0) {
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
    if (dirfd < 0 || dirfd >= PROCESS_MAX_FDS || !process->files->fds[dirfd]) return NULL;
    struct file *file = process->files->fds[dirfd];
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
        int permitted = cred_may_write_parent(path);
        if (permitted != 0) return permitted;
        node = vfs_create_file_node(path, mode_after_umask(mode));
        if (!node) return -ENOENT;
    } else if (!node) return -ENOENT;
    else if ((flags & O_CREAT) && (flags & O_EXCL)) return -EEXIST;
    else {
        uint32_t want = 0;
        if (!(flags & O_PATH)) {
            uint32_t access_mode = flags & O_ACCMODE;
            if (access_mode == O_RDONLY || access_mode == O_RDWR) want |= CRED_READ;
            if (access_mode == O_WRONLY || access_mode == O_RDWR) want |= CRED_WRITE;
            if (flags & O_TRUNC) want |= CRED_WRITE;
        }
        int permitted = cred_may_path(path, node, want);
        if (permitted != 0) return permitted;
    }

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
    if (!process || oldfd < 0 || oldfd >= PROCESS_MAX_FDS || !process->files->fds[oldfd]) return -EBADF;
    if (minimum < 0 || minimum >= PROCESS_MAX_FDS) return -EINVAL;
    file_ref(process->files->fds[oldfd]);
    int result = process_install_file_flags(process, process->files->fds[oldfd], minimum,
        cloexec ? PROCESS_FD_CLOEXEC : 0);
    if (result < 0) {
        file_unref(process->files->fds[oldfd]);
        return -EMFILE;
    }
    return result;
}

static int64_t sys_dup_to(int oldfd, int newfd, int cloexec, int reject_same) {
    struct process *process = process_current();
    if (!process || oldfd < 0 || oldfd >= PROCESS_MAX_FDS || !process->files->fds[oldfd]) return -EBADF;
    if (newfd < 0 || newfd >= PROCESS_MAX_FDS) return -EBADF;
    if (oldfd == newfd) return reject_same ? -EINVAL : newfd;
    if (process->files->fds[newfd]) process_close_fd(process, newfd);
    file_ref(process->files->fds[oldfd]);
    process->files->fds[newfd] = process->files->fds[oldfd];
    process->files->fd_flags[newfd] = cloexec ? PROCESS_FD_CLOEXEC : 0;
    return newfd;
}

static int64_t sys_pipe(uint64_t user_fds, int flags) {
    if (flags & ~(O_CLOEXEC | O_NONBLOCK)) return -EINVAL;
    struct file *read_end;
    struct file *write_end;
    if (pipe_create(&read_end, &write_end) != 0) return -EMFILE;
    /* The access mode is not decoration: fcntl(F_GETFL) is how a program asks
       which way a descriptor goes, and with both ends reading back O_RDONLY
       GLib decided a GIOChannel on the write end was not writable and refused
       every write to it. That is what closed LightDM's greeter channel. */
    read_end->flags = (uint32_t)(flags & O_NONBLOCK) | O_RDONLY;
    write_end->flags = (uint32_t)(flags & O_NONBLOCK) | O_WRONLY;
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
    if (!process || fd < 0 || fd >= PROCESS_MAX_FDS || !process->files->fds[fd]) return NULL;
    struct file *file = process->files->fds[fd];
    return file->kind == FILE_KIND_SOCKET ? file->socket : NULL;
}

static struct inet_socket *inet_socket_from_fd(int fd) {
    struct process *process = process_current();
    if (!process || fd < 0 || fd >= PROCESS_MAX_FDS || !process->files->fds[fd]) return NULL;
    struct file *file = process->files->fds[fd];
    return file->kind == FILE_KIND_INET_SOCKET ? file->inet_socket : NULL;
}

static struct netlink_socket *netlink_socket_from_fd(int fd) {
    struct process *process = process_current();
    if (!process || fd < 0 || fd >= PROCESS_MAX_FDS || !process->files->fds[fd]) return NULL;
    struct file *file = process->files->fds[fd];
    return file->kind == FILE_KIND_NETLINK_SOCKET ? file->netlink_socket : NULL;
}

static int64_t sys_socket(int domain, int type, int protocol) {
    int base_type = type & 0xF;
    int type_flags = type & ~0xF;
    if (type_flags & ~(SOCK_NONBLOCK | SOCK_CLOEXEC)) return -EINVAL;
    struct process *process = process_current();
    if (domain == TUNIX_AF_UNIX) {
        /*
         * SOCK_DGRAM shares the seqpacket implementation: both keep message
         * boundaries, and the difference left over -- sending to a path
         * without connecting first -- is not something anything here does.
         *
         * Refusing it outright was worse than an approximation. musl builds
         * if_indextoname() out of an AF_UNIX SOCK_DGRAM socket and an ioctl
         * on it, using the socket purely as a handle, so a program asking
         * which interface a route belongs to got nothing at all -- which is
         * how fastfetch came to skip every interface it had just enumerated.
         */
        if ((base_type != TUNIX_SOCK_STREAM && base_type != TUNIX_SOCK_SEQPACKET &&
             base_type != TUNIX_SOCK_DGRAM) || protocol != 0) return -EOPNOTSUPP;
        struct unix_socket *socket =
            unix_socket_create(base_type != TUNIX_SOCK_STREAM);
        if (!socket) return -ENOMEM;
        /* The identity the peer will read back through SO_PEERCRED. D-Bus
           authenticates with it: a client announces its uid over EXTERNAL and
           the bus rejects it unless the socket agrees. */
        unix_socket_set_credentials(socket, process ? (int32_t)process->pid : 0,
                                    process ? process->cred.euid : 0,
                                    process ? process->cred.egid : 0);
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
    /* A datagram pair as well as a stream one. Both ends are connected and
       neither can lose a message, so it is the same object as a seqpacket pair
       -- which is what a udev daemon asks for and used to be refused. */
    if (domain != TUNIX_AF_UNIX || protocol != 0 ||
        (base_type != TUNIX_SOCK_STREAM && base_type != TUNIX_SOCK_SEQPACKET &&
         base_type != TUNIX_SOCK_DGRAM))
        return -EOPNOTSUPP;
    struct unix_socket *first = NULL;
    struct unix_socket *second = NULL;
    int status = unix_socket_pair(&first, &second,
                                  base_type != TUNIX_SOCK_STREAM);
    if (status < 0) return status;
    struct process *process = process_current();
    int32_t pid = process ? (int32_t)process->pid : 0;
    uint32_t uid = process ? process->cred.euid : 0;
    uint32_t gid = process ? process->cred.egid : 0;
    unix_socket_set_credentials(first, pid, uid, gid);
    unix_socket_set_credentials(second, pid, uid, gid);
    struct file *first_file = file_create_socket(first);
    struct file *second_file = file_create_socket(second);
    if (!first_file || !second_file) {
        if (first_file) file_unref(first_file); else unix_socket_unref(first);
        if (second_file) file_unref(second_file); else unix_socket_unref(second);
        return -ENOMEM;
    }
    /* Both ends go both ways; same reason the pipe ends record theirs. */
    first_file->flags = (uint32_t)(type_flags & SOCK_NONBLOCK) | O_RDWR;
    second_file->flags = (uint32_t)(type_flags & SOCK_NONBLOCK) | O_RDWR;
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
        if (status < 0) return status;
        /* A name in the filesystem for every socket bound to a path, as Linux
           gives one. The abstract namespace -- a leading NUL -- has no such
           name and gets none. The node is refused if the path is taken, which
           is why a daemon unlinks its socket before binding it. */
        int named = address.path[0] != 0;
        if (named && vfs_lookup_nofollow(address.path)) return -EADDRINUSE;
        status = unix_socket_bind(unix_value, &address, (size_t)length);
        if (status == 0 && named) {
            struct process *self = process_current();
            uint32_t mode = 0777U & ~(self ? self->umask : 0U);
            (void)vfs_create_socket_node(address.path, mode);
        }
        return status;
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
    if (socket) return unix_socket_listen(socket, backlog);
    struct inet_socket *inet_value = inet_socket_from_fd(fd);
    return inet_value ? inet_socket_listen(inet_value, backlog) : -EBADF;
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
    if (!process || fd < 0 || fd >= PROCESS_MAX_FDS || !process->files->fds[fd]) return -EBADF;
    struct file *file = process->files->fds[fd];
    if (file->kind == FILE_KIND_SOCKET) return unix_socket_shutdown(file->socket, how);
    if (file->kind == FILE_KIND_INET_SOCKET) return inet_socket_shutdown(file->inet_socket, how);
    return -ENOTSOCK;
}

/* Hand the finished socket to a descriptor. Shared by both address families,
   which differ only in what the file wraps and what an address looks like. */
static int64_t install_accepted(struct file *file, int flags,
                                const void *address, size_t address_length,
                                uint64_t user_address, uint64_t user_length) {
    file->flags = (uint32_t)(flags & O_NONBLOCK);
    int new_fd = process_install_file_flags(process_current(), file, 0,
        (flags & O_CLOEXEC) ? PROCESS_FD_CLOEXEC : 0);
    if (new_fd < 0) {
        file_unref(file);
        return -EMFILE;
    }
    /* The peer address is written last: failing to copy it out must not undo
       an accept that has already taken the connection off the queue. */
    if (user_address && user_length && address_length) {
        uint32_t capacity = 0;
        if (copy_from_user(&capacity, user_length, sizeof(capacity)) == 0) {
            size_t copy = capacity < address_length ? capacity : address_length;
            if (copy) (void)copy_to_user(user_address, address, copy);
            uint32_t reported = (uint32_t)address_length;
            (void)copy_to_user(user_length, &reported, sizeof(reported));
        }
    }
    return new_fd;
}

static int64_t sys_accept(int fd, uint64_t user_address, uint64_t user_length, int flags) {
    if (flags & ~(O_NONBLOCK | O_CLOEXEC)) return -EINVAL;
    struct unix_socket *listener = socket_from_fd(fd);
    if (listener) {
        if (!unix_socket_is_listener(listener)) return -EINVAL;
        struct unix_socket *accepted = unix_socket_accept(listener);
        if (!accepted) return -EAGAIN;
        struct file *file = file_create_socket(accepted);
        if (!file) {
            unix_socket_unref(accepted);
            return -ENOMEM;
        }
        return install_accepted(file, flags, NULL, 0, user_address, user_length);
    }

    struct inet_socket *inet_listener = inet_socket_from_fd(fd);
    if (!inet_listener) return -EBADF;
    if (!inet_socket_is_listener(inet_listener)) return -EINVAL;
    struct inet_socket *accepted = inet_socket_accept(inet_listener);
    if (!accepted) return -EAGAIN;
    struct tunix_sockaddr_in peer;
    size_t peer_length = sizeof(peer);
    if (inet_socket_getpeername(accepted, &peer, &peer_length) != 0) peer_length = 0;
    struct file *file = file_create_inet_socket(accepted);
    if (!file) {
        inet_socket_unref(accepted);
        return -ENOMEM;
    }
    return install_accepted(file, flags, &peer, peer_length, user_address, user_length);
}

/*
 * accept(2) on a blocking socket waits for a connection; only a non-blocking
 * one answers EAGAIN. Everything on the image that accepts -- X11, Wayland,
 * D-Bus -- polls first and calls accept only once a connection is already
 * pending, so a plain blocking accept had never been tried until Python did
 * one and got EAGAIN out of a socket it had not asked to be non-blocking.
 *
 * The retry is the same rewind recvfrom uses: step the frame back onto the
 * syscall instruction, put the number back in rax and yield, so the process
 * re-runs it after other work has had a chance to connect.
 */
static void accept_or_block(struct syscall_frame *frame, uint64_t syscall_number,
                            int fd, uint64_t user_address, uint64_t user_length,
                            int flags) {
    int64_t result = sys_accept(fd, user_address, user_length, flags);
    struct process *process = process_current();
    struct file *file = process && fd >= 0 && fd < PROCESS_MAX_FDS
                            ? process->files->fds[fd] : NULL;
    if (result == -EAGAIN && file && !(file->flags & O_NONBLOCK)) {
        frame->user_rip -= 2U;
        frame->rax = syscall_number;
        process->syscall_rewound = 1;
        process_yield_from_syscall(frame);
        return;
    }
    frame->rax = (uint64_t)result;
}

static int64_t sys_sendto(int fd, uint64_t user_data, size_t length, int flags,
                          uint64_t user_address, uint64_t address_length) {
    /* send()/sendto() on a unix socket: GLib's GDBus writes to the D-Bus session
       bus this way, so a stream write has to work here as well as sendmsg. send()
       carries no ancillary data; the destination address is ignored on a
       connected socket. */
    struct unix_socket *unix_value = socket_from_fd(fd);
    if (unix_value) {
        (void)flags;
        (void)user_address;
        (void)address_length;
        if (length > 4096U) length = 4096U;
        uint8_t data[4096];
        if (length && copy_from_user(data, user_data, length) != 0) return -EFAULT;
        return unix_socket_write(unix_value, length, data);
    }
    struct netlink_socket *netlink = netlink_socket_from_fd(fd);
    if (netlink) {
        if (length > 4096U) return -EMSGSIZE;
        uint8_t request[4096];
        if (length && copy_from_user(request, user_data, length) != 0) return -EFAULT;
        /* The destination decides who a uevent message reaches, so it has to
           come along here as it does through sendmsg. */
        struct tunix_sockaddr_nl destination;
        int addressed = user_address && address_length >= sizeof(destination) &&
            copy_from_user(&destination, user_address, sizeof(destination)) == 0;
        return netlink_socket_sendto(netlink, request, length, flags,
                                     addressed ? &destination : NULL,
                                     addressed ? sizeof(destination) : 0U);
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
    /* recv()/recvfrom() on a unix socket: libxcb reads the X setup reply this way
       (read_block -> recv), so a stream read has to work here, not just recvmsg.
       recv() carries no ancillary data, so read plain bytes and leave any pending
       SCM_RIGHTS in place for a later recvmsg. */
    struct unix_socket *unix_value = socket_from_fd(fd);
    if (unix_value) {
        (void)flags;
        if (length > 4096U) length = 4096U;
        uint8_t data[4096];
        int64_t result = unix_socket_read(unix_value, length, data);
        if (result < 0) return result;
        if (result && copy_to_user(user_data, data, (size_t)result) != 0) return -EFAULT;
        if (user_address_length) {
            uint32_t zero = 0;
            if (copy_to_user(user_address_length, &zero, sizeof(zero)) != 0) return -EFAULT;
        }
        return result;
    }
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
    if (netlink) {
        /* The destination matters here: udevd addresses its re-announcement to
           a multicast group rather than to a port, and the group is in the
           sockaddr rather than in the message. */
        struct tunix_sockaddr_nl destination;
        int addressed = message.name && message.name_length >= sizeof(destination) &&
            copy_from_user(&destination, message.name, sizeof(destination)) == 0;
        return netlink_socket_sendto(netlink, data, length, flags,
                                     addressed ? &destination : NULL,
                                     addressed ? sizeof(destination) : 0U);
    }

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

/*
 * SCM_CREDENTIALS for a netlink datagram.
 *
 * udev's monitor will not look at a message that arrives without one, and
 * throws away any whose sender is not root -- so a uevent the kernel sent has
 * to say so, with the kernel's own pid of zero and uid of zero.
 */
static int write_netlink_control(struct linux_msghdr *message,
                                 struct netlink_socket *socket) {
    if (!netlink_socket_get_passcred(socket)) {
        message->control_length = 0;
        return 0;
    }
    size_t credentials_length =
        sizeof(struct linux_cmsghdr) + sizeof(struct linux_ucred);
    if (!message->control || message->control_length < cmsg_align(credentials_length)) {
        message->flags |= MSG_CTRUNC;
        message->control_length = 0;
        return 0;
    }
    struct netlink_credentials sender;
    netlink_socket_last_credentials(socket, &sender);
    struct linux_cmsghdr header = {credentials_length, SOL_SOCKET, SCM_CREDENTIALS};
    struct linux_ucred credentials = {sender.pid, sender.uid, sender.gid};
    if (copy_to_user(message->control, &header, sizeof(header)) != 0 ||
        copy_to_user(message->control + sizeof(header), &credentials,
                     sizeof(credentials)) != 0) return -EFAULT;
    message->control_length = credentials_length;
    return 0;
}

static int write_unix_control(struct linux_msghdr *message,
                              struct unix_socket *socket,
                              struct file **files, size_t file_count,
                              int receive_flags) {
    struct unix_credentials peer = {0, 0, 0};
    int include_credentials = unix_socket_get_passcred(socket);
    if (include_credentials) unix_socket_last_sender(socket, &peer);
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

/*
 * Several messages in one call.
 *
 * The whole of it is a loop around sendmsg, which is what Linux does too: the
 * saving is the syscalls, not the sending. It is here because glibc's resolver
 * puts the A and the AAAA query for one name into a single sendmmsg, and does
 * not fall back when the call is refused -- so every name lookup made by a
 * program that asks for both families at once failed, without a packet
 * reaching the wire. xbps was one; `getent hosts`, which asks for one family,
 * was not, which is what made it look like the network was intermittent.
 *
 * The return is the number of messages accepted. An error is only reported
 * when the first one fails: a partial send is a success with a smaller count,
 * and the caller finds out by reading msg_len.
 */
static int64_t sys_sendmmsg(int fd, uint64_t user_vector, unsigned count, int flags) {
    if (!user_vector) return -EFAULT;
    /* UIO_MAXIOV, the same ceiling Linux puts on it. */
    if (count > 1024U) count = 1024U;

    unsigned sent = 0;
    for (; sent < count; sent++) {
        uint64_t element = user_vector + (uint64_t)sent * sizeof(struct linux_mmsghdr);
        int64_t result = sys_sendmsg(fd, element, flags);
        if (result < 0) return sent ? (int64_t)sent : result;
        uint32_t length = (uint32_t)result;
        if (copy_to_user(element + offsetof(struct linux_mmsghdr, msg_len),
                         &length, sizeof(length)) != 0)
            return sent ? (int64_t)sent : -EFAULT;
    }
    return (int64_t)sent;
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
        int status = write_netlink_control(&message, netlink);
        if (status < 0) return status;
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
    struct netlink_socket *netlink_option = netlink_socket_from_fd(fd);
    if (netlink_option) {
        /* SO_PASSCRED is the one that means something: udev's monitor sets it
           and then discards every message that arrives without a sender. */
        if (level == SOL_SOCKET && option == SO_PASSCRED && length >= sizeof(int32_t)) {
            int32_t enabled;
            if (copy_from_user(&enabled, user_value, sizeof(enabled)) != 0) return -EFAULT;
            netlink_socket_set_passcred(netlink_option, enabled != 0);
            return 0;
        }
        /* iproute2 sets SO_SNDBUF/SO_RCVBUF and a few SOL_NETLINK options while
           opening the socket; accept them so rtnl_open() does not bail out. */
        (void)user_value; (void)length;
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
    if (!process || fd < 0 || fd >= PROCESS_MAX_FDS || !process->files->fds[fd]) return -EBADF;
    struct file *file = process->files->fds[fd];
    /* This is how a memfd gets its size: a Wayland client creates one and
       immediately ftruncates it to the buffer size before mapping it. */
    if (file->kind == FILE_KIND_MEMFD)
        return memfd_truncate(file->memfd, length) == 0 ? 0 : -ENOMEM;
    if (file->kind != FILE_KIND_VFS || !file->node) return -EINVAL;
    return vfs_truncate(file->node, length) == 0 ? 0 : -EIO;
}

/*
 * fallocate(2), enough of it to reserve space.
 *
 * Wayland clients allocate their shm buffers with memfd_create() followed by
 * posix_fallocate(), which musl implements with this syscall. Without it the
 * call returns ENOSYS and weston's clients die with "creating a buffer file
 * failed: Function not implemented" -- so this is what stands between a
 * compositor that starts and one that can draw.
 *
 * Only mode 0 (allocate, growing the file if needed) is supported. The punching
 * and collapsing modes describe operations on extents that neither memfd nor
 * the ext2 driver models, so they are refused rather than silently ignored.
 */
static int64_t sys_fallocate(int fd, int mode, uint64_t offset, uint64_t length) {
    struct process *process = process_current();
    if (!process || fd < 0 || fd >= PROCESS_MAX_FDS || !process->files->fds[fd]) return -EBADF;
    if (mode != 0) return -EOPNOTSUPP;
    if ((int64_t)offset < 0 || (int64_t)length < 0) return -EINVAL;
    if (length > UINT64_MAX - offset) return -EFBIG;

    struct file *file = process->files->fds[fd];
    uint64_t needed = offset + length;

    if (file->kind == FILE_KIND_MEMFD) {
        /* Growing only: fallocate never shrinks. */
        if (needed <= memfd_size(file->memfd)) return 0;
        return memfd_truncate(file->memfd, needed) == 0 ? 0 : -ENOSPC;
    }
    if (file->kind == FILE_KIND_VFS && file->node) {
        if (needed <= file->node->length) return 0;
        return vfs_truncate(file->node, needed) == 0 ? 0 : -ENOSPC;
    }
    return -ENODEV;
}

/* Resolves the descriptor and takes the lock; the blocking retry lives in the
   dispatcher, where the syscall frame is available to rewind. */
/* access(2) judges with the real ids, faccessat(AT_EACCESS) with the effective
   ones; the file checks always read fsuid/fsgid, so swap them for the former. */
static int64_t sys_faccess_at(int dirfd, uint64_t user_path, int mode, int flags) {
    if (flags & ~(AT_EACCESS | AT_SYMLINK_NOFOLLOW)) return -EINVAL;
    if (mode & ~7) return -EINVAL;
    char path[256];
    int status = copy_path_at(dirfd, user_path, path);
    if (status != 0) return status;
    struct vfs_node *node = (flags & AT_SYMLINK_NOFOLLOW) ? vfs_lookup_nofollow(path)
                                                          : vfs_lookup(path);
    if (!node) return -ENOENT;

    uint32_t want = 0;
    if (mode & 4) want |= CRED_READ;
    if (mode & 2) want |= CRED_WRITE;
    if (mode & 1) want |= CRED_EXEC;
    if ((want & CRED_WRITE) && (node->flags & VFS_READONLY)) return -EROFS;

    struct credentials *cred = cred_current();
    uint32_t saved_uid = 0, saved_gid = 0;
    int swapped = cred && !(flags & AT_EACCESS);
    if (swapped) {
        saved_uid = cred->fsuid;
        saved_gid = cred->fsgid;
        cred->fsuid = cred->uid;
        cred->fsgid = cred->gid;
    }
    int permitted = cred_may_path(path, node, want);
    if (swapped) {
        cred->fsuid = saved_uid;
        cred->fsgid = saved_gid;
    }
    return permitted;
}

static int64_t sys_getresuid(uint64_t real_user, uint64_t effective_user,
                             uint64_t saved_user, int group) {
    const struct credentials *cred = cred_current();
    if (!cred) return -EINVAL;
    uint32_t values[3];
    if (group) { values[0] = cred->gid; values[1] = cred->egid; values[2] = cred->sgid; }
    else { values[0] = cred->uid; values[1] = cred->euid; values[2] = cred->suid; }
    if (copy_to_user(real_user, &values[0], sizeof(values[0])) != 0 ||
        copy_to_user(effective_user, &values[1], sizeof(values[1])) != 0 ||
        copy_to_user(saved_user, &values[2], sizeof(values[2])) != 0)
        return -EFAULT;
    return 0;
}

static int64_t sys_getgroups(int64_t size, uint64_t user_list) {
    const struct credentials *cred = cred_current();
    if (!cred) return 0;
    if (size < 0) return -EINVAL;
    if (size == 0) return (int64_t)cred->group_count;
    if ((uint32_t)size < cred->group_count) return -EINVAL;
    if (cred->group_count &&
        copy_to_user(user_list, cred->groups, cred->group_count * sizeof(uint32_t)) != 0)
        return -EFAULT;
    return (int64_t)cred->group_count;
}

static int64_t sys_setgroups(int64_t size, uint64_t user_list) {
    if (size < 0 || size > CRED_MAX_GROUPS) return -EINVAL;
    uint32_t groups[CRED_MAX_GROUPS];
    if (size && copy_from_user(groups, user_list, (size_t)size * sizeof(uint32_t)) != 0)
        return -EFAULT;
    return cred_set_groups((uint32_t)size, groups);
}

static int64_t sys_flock(int fd, int operation) {
    struct process *process = process_current();
    if (!process || fd < 0 || fd >= PROCESS_MAX_FDS || !process->files->fds[fd]) return -EBADF;
    return file_flock(process->files->fds[fd], operation);
}

/*
 * POSIX advisory locks, as fcntl(F_SETLK) asks for them.
 *
 * SQLite is what needs these: it locks single bytes and small ranges to
 * arbitrate between connections, and with fcntl answering EINVAL every
 * database file it opened failed with "disk I/O error". flock(2) is a
 * different lock space and does not serve.
 *
 * The range is deliberately ignored and the whole file is locked instead.
 * That is coarser than POSIX promises, and the direction of the error matters:
 * locking more than asked can only ever refuse a lock that should have been
 * granted, never grant one that should have been refused. What it costs is
 * concurrency between processes sharing a database -- one may see EAGAIN where
 * real byte ranges would not have collided. Within a single process nothing is
 * lost, because POSIX locks belong to the process and a process never
 * conflicts with itself: SQLite holding SHARED and then taking RESERVED is one
 * owner replacing its own lock, which is exactly what happens here.
 */
static int64_t vfs_posix_lock(struct vfs_node *node, int type, uint64_t pid) {
    if (type == F_UNLCK) {
        if (node->posix_lock_pid == pid) {
            node->posix_lock_pid = 0;
            node->posix_lock_write = 0;
        }
        return 0;
    }
    if (type != F_RDLCK && type != F_WRLCK) return -EINVAL;
    if (node->posix_lock_pid && node->posix_lock_pid != pid) return -EAGAIN;
    node->posix_lock_pid = pid;
    node->posix_lock_write = type == F_WRLCK;
    return 0;
}

static int64_t sys_fcntl_lock(int fd, int command, uint64_t user_lock) {
    struct process *process = process_current();
    if (!process || fd < 0 || fd >= PROCESS_MAX_FDS || !process->files->fds[fd]) return -EBADF;
    struct file *file = process->files->fds[fd];
    if (file->kind != FILE_KIND_VFS || !file->node) return -EINVAL;

    struct linux_flock lock;
    if (copy_from_user(&lock, user_lock, sizeof(lock)) != 0) return -EFAULT;

    if (command == F_GETLK) {
        /* Only a holder that is somebody else is a conflict worth reporting. */
        if (file->node->posix_lock_pid && file->node->posix_lock_pid != process->tgid) {
            lock.type = file->node->posix_lock_write ? F_WRLCK : F_RDLCK;
            lock.pid = (int32_t)file->node->posix_lock_pid;
        } else {
            lock.type = F_UNLCK;
        }
        return copy_to_user(user_lock, &lock, sizeof(lock)) == 0 ? 0 : -EFAULT;
    }
    /* The thread group, not the thread: POSIX locks belong to the process. */
    return vfs_posix_lock(file->node, lock.type, process->tgid);
}

static int64_t sys_fsync(int fd) {
    struct process *process = process_current();
    if (!process || fd < 0 || fd >= PROCESS_MAX_FDS || !process->files->fds[fd]) return -EBADF;
    struct file *file = process->files->fds[fd];
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
    if (!process || fd < 0 || fd >= PROCESS_MAX_FDS || !process->files->fds[fd]) return -EBADF;
    /*
     * An ioctl request is 32 bits. libc declares ioctl() as taking an int, so
     * any request with the read direction set -- 0x8xxxxxxx, which is every
     * _IOR: TIOCGPTN, EVIOCGVERSION, RTC_RD_TIME -- is negative as an int and
     * arrives here sign-extended to 64 bits. Linux truncates because its
     * handler takes an unsigned int; do the same, or every exact comparison
     * below silently misses and the caller is told ENOTTY.
     */
    request &= 0xFFFFFFFFUL;
    struct file *file = process->files->fds[fd];
    /*
     * FIONBIO is the other way to say O_NONBLOCK, and it belongs to the
     * descriptor rather than to whatever is behind it -- so it is answered
     * here, before anything gets to interpret the argument. CPython's
     * socket.settimeout() uses it in preference to fcntl, and on an inet
     * socket it used to fall through to the interface ioctls, which read the
     * argument as an interface name and answered EADDRNOTAVAIL.
     */
    if (request == FIONBIO) {
        int32_t enabled;
        if (!user_argument ||
            copy_from_user(&enabled, user_argument, sizeof(enabled)) != 0) return -EFAULT;
        if (enabled) file->flags |= (uint32_t)O_NONBLOCK;
        else file->flags &= ~(uint32_t)O_NONBLOCK;
        return 0;
    }
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
    if (file->kind == FILE_KIND_INPUT && file->node) {
        /* Tunix's own EVIOCGINFO is answered by the node; everything in the
           'E' group is evdev and needs the reader, because EVIOCSCLOCKID and
           EVIOCGRAB belong to the descriptor rather than the device. */
        if (((request >> 8) & 0xFFU) == (unsigned)'E')
            return input_reader_ioctl(file->input_reader,
                                      (unsigned)(uintptr_t)file->node->data,
                                      request, user_argument);
        if (file->node->ioctl)
            return file->node->ioctl(file->node, request, user_argument);
    }
    if (file->kind == FILE_KIND_FRAMEBUFFER)
        return framebuffer_file_ioctl(file, request, user_argument);
    /*
     * The SIOCGIF* device ioctls are a property of the machine's interfaces,
     * not of the socket they arrive on, and Linux answers them on any socket
     * at all. That is not a nicety: musl implements if_indextoname() by
     * opening an AF_UNIX socket and asking it for SIOCGIFNAME, so a program
     * that resolves the interface behind a route -- fastfetch's Local IP
     * does exactly that -- never gets an answer if only inet sockets reply.
     */
    if (file->kind == FILE_KIND_SOCKET && (request & 0xFF00U) == 0x8900U) {
        uint8_t argument[40];
        if (!user_argument || copy_from_user(argument, user_argument, sizeof(argument)) != 0)
            return -EFAULT;
        int status = net_interface_ioctl(request, argument);
        if (status < 0) return status;
        return copy_to_user(user_argument, argument, sizeof(argument)) == 0 ? 0 : -EFAULT;
    }
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

    /*
     * A character device that is not a terminal. Every terminal there is --
     * /dev/ttyN, /dev/tty0, /dev/tty, /dev/console -- carries vt_node_ioctl and
     * was answered above; the pseudo-terminals have their own path further up.
     * Saying so plainly here is what makes isatty() tell the truth about
     * /dev/null, which the old catch-all did not.
     */
    return -ENOTTY;
}

static void fill_stat(struct vfs_node *node, struct linux_stat *stat) {
    memset(stat, 0, sizeof(*stat));
    stat->st_ino = node->inode;
    stat->st_nlink = node->links ? node->links : 1;
    stat->st_uid = node->uid;
    stat->st_gid = node->gid;
    stat->st_size = (int64_t)node->length;
    stat->st_blksize = 4096;
    stat->st_blocks = (int64_t)((node->length + 511) / 512);
    uint32_t kind = node->flags & 0xFFU;
    uint32_t type = kind == VFS_DIRECTORY ? 0040000U :
                    (kind == VFS_CHARDEVICE ? 0020000U :
                    (kind == VFS_BLOCKDEVICE ? 0060000U :
                    (kind == VFS_SYMLINK ? 0120000U :
                    (kind == VFS_PIPE ? 0010000U :
                    (kind == VFS_SOCKET ? 0140000U : 0100000U)))));
    /* 07777, not 0777: the setuid, setgid and sticky bits are part of the mode
       and a caller that cannot see them cannot tell su from any other program. */
    stat->st_mode = type | (node->mode & 07777U);
    if (kind == VFS_CHARDEVICE || kind == VFS_BLOCKDEVICE) {
        /* Linux's encoding: 20 bits of minor split around 12 bits of major. */
        uint64_t major = node->dev_major;
        uint64_t minor = node->dev_minor;
        stat->st_rdev = ((major & 0xFFFULL) << 8) | (minor & 0xFFULL) |
                        ((major & ~0xFFFULL) << 32) | ((minor & ~0xFFULL) << 12);
    }
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
            out->f_blocks = pmm_usable_page_count();
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
    out->f_blocks = pmm_usable_page_count();
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
    if (!process || fd < 0 || fd >= PROCESS_MAX_FDS || !process->files->fds[fd]) return -EBADF;
    struct file *file = process->files->fds[fd];
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
    if (!process || fd < 0 || fd >= PROCESS_MAX_FDS || !process->files->fds[fd]) return -EBADF;
    struct linux_stat stat;
    if (stat_from_file(process->files->fds[fd], &stat) != 0) return -EBADF;
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
    /* statx splits what stat packs into one field. */
    out->stx_rdev_major = (uint32_t)(((basic.st_rdev >> 8) & 0xFFFU) |
                                     ((basic.st_rdev >> 32) & ~0xFFFU));
    out->stx_rdev_minor = (uint32_t)((basic.st_rdev & 0xFFU) |
                                     ((basic.st_rdev >> 12) & ~0xFFU));
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
        if (!process || dirfd >= PROCESS_MAX_FDS || !process->files->fds[dirfd]) return -EBADF;
        if (stat_from_file(process->files->fds[dirfd], &basic) != 0) return -EBADF;
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
    if (!process || fd < 0 || fd >= PROCESS_MAX_FDS || !process->files->fds[fd]) return -EBADF;
    struct file *file = process->files->fds[fd];
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
    if (!process || fd < 0 || fd >= PROCESS_MAX_FDS || !process->files->fds[fd]) return -EBADF;
    struct file *file = process->files->fds[fd];
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

/* Reference the new directory before releasing the old one: they can be the
   same node, and dropping the last reference first would free it. */
static void set_cwd(struct process *process, struct vfs_node *node) {
    struct vfs_node *previous = process->cwd;
    vfs_node_ref(node);
    process->cwd = node;
    vfs_node_unref(previous);
}

static int64_t sys_chdir(uint64_t user_path) {
    char path[256];
    int status = copy_path_at(AT_FDCWD, user_path, path);
    if (status != 0) return status;
    struct vfs_node *node = vfs_lookup(path);
    if (!node) return -ENOENT;
    if ((node->flags & 0xFFU) != VFS_DIRECTORY) return -ENOTDIR;
    int permitted = cred_may_path(path, node, CRED_EXEC);
    if (permitted != 0) return permitted;
    set_cwd(process_current(), node);
    return 0;
}

static int64_t sys_fchdir(int fd) {
    struct process *process = process_current();
    if (!process || fd < 0 || fd >= PROCESS_MAX_FDS || !process->files->fds[fd]) return -EBADF;
    struct file *file = process->files->fds[fd];
    if (file->kind != FILE_KIND_VFS || !file->node || (file->node->flags & 0xFFU) != VFS_DIRECTORY) return -ENOTDIR;
    set_cwd(process, file->node);
    return 0;
}

static int64_t sys_mkdir_at(int dirfd, uint64_t user_path, uint64_t mode) {
    char path[256];
    int status = copy_path_at(dirfd, user_path, path);
    if (status != 0) return status;
    if (vfs_lookup(path)) return -EEXIST;
    int permitted = cred_may_write_parent(path);
    if (permitted != 0) return permitted;
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
    int permitted = cred_may_remove(path, node);
    if (permitted != 0) return permitted;
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
    int permitted = cred_may_remove(old_path, node);
    if (permitted != 0) return permitted;
    struct vfs_node *existing = vfs_lookup_nofollow(new_path);
    permitted = existing ? cred_may_remove(new_path, existing)
                         : cred_may_write_parent(new_path);
    if (permitted != 0) return permitted;
    return vfs_rename(old_path, new_path) == 0 ? 0 : -EIO;
}

/*
 * link(2). The name being created is an entry in a directory, so the write
 * permission that matters is on that directory rather than on the file: a file
 * nobody may write can still be given another name by whoever owns the
 * directory it is going into, which is how package managers unpack.
 */
static int64_t sys_link_at(int old_dirfd, uint64_t user_old_path,
                           int new_dirfd, uint64_t user_new_path, int flags) {
    if (flags & ~AT_SYMLINK_FOLLOW) return -EINVAL;
    char old_path[256], new_path[256];
    int status = copy_path_at(old_dirfd, user_old_path, old_path);
    if (status != 0) return status;
    status = copy_path_at(new_dirfd, user_new_path, new_path);
    if (status != 0) return status;

    struct vfs_node *node = (flags & AT_SYMLINK_FOLLOW) ? vfs_lookup(old_path)
                                                        : vfs_lookup_nofollow(old_path);
    if (!node) return -ENOENT;
    /* Linking a directory would let a tree contain itself, and nothing in the
       kernel walks a cycle safely. */
    if ((node->flags & 0xFFU) == VFS_DIRECTORY) return -EPERM;
    if (node->flags & VFS_READONLY) return -EROFS;
    if (vfs_lookup_nofollow(new_path)) return -EEXIST;
    int permitted = cred_may_write_parent(new_path);
    if (permitted != 0) return permitted;
    return vfs_link(node, new_path) == 0 ? 0 : -EIO;
}

/*
 * mount(2). Only root may change what the filesystem tree looks like, since a
 * mount is visible to every process on the machine -- there are no mount
 * namespaces here to confine it to the caller.
 */
static int64_t sys_mount(uint64_t user_source, uint64_t user_target,
                         uint64_t user_type, uint64_t flags, uint64_t user_data) {
    (void)user_data;                     /* no filesystem takes options yet */
    const struct credentials *cred = cred_current();
    if (cred && cred->euid != 0) return -EPERM;
    if (flags > 0xFFFFFFFFULL) return -EINVAL;

    char source[256], target[256], type[64];
    source[0] = '\0';
    type[0] = '\0';
    if (user_source && copy_string_from_user(source, sizeof(source), user_source) < 0)
        return -EFAULT;
    if (copy_string_from_user(target, sizeof(target), user_target) < 0) return -EFAULT;
    if (user_type && copy_string_from_user(type, sizeof(type), user_type) < 0)
        return -EFAULT;
    return vfs_mount(user_source ? source : NULL, target,
                     user_type ? type : "", (uint32_t)flags);
}

/* umount2(2). The flags Linux takes are all about how hard to try when the
   filesystem is busy, and nothing here can be busy in that sense. */
static int64_t sys_umount2(uint64_t user_target, int flags) {
    (void)flags;
    const struct credentials *cred = cred_current();
    if (cred && cred->euid != 0) return -EPERM;
    char target[256];
    if (copy_string_from_user(target, sizeof(target), user_target) < 0) return -EFAULT;
    return vfs_umount(target);
}

/*
 * mknod(2), for the one file type it can make: a FIFO.
 *
 * Device nodes are refused rather than faked. /dev is built by the kernel from
 * the devices it actually found, so a node made here would name a driver that
 * is not behind it; MAKEDEV scripts get EPERM, which is what they get on a
 * system with devtmpfs too. Making a regular file is allowed because mknod(2)
 * says it is and it costs nothing.
 */
static int64_t sys_mknodat(int dirfd, uint64_t user_path, uint32_t mode,
                           uint64_t device) {
    (void)device;
    char path[256];
    int status = copy_path_at(dirfd, user_path, path);
    if (status != 0) return status;

    uint32_t type = mode & 0170000U;
    if (type != 0010000U && type != 0100000U && type != 0) return -EPERM;
    if (vfs_lookup_nofollow(path)) return -EEXIST;
    int permitted = cred_may_write_parent(path);
    if (permitted != 0) return permitted;

    if (type == 0010000U)
        return vfs_create_fifo(path, mode & 07777U) ? 0 : -EIO;
    return vfs_create_file_node(path, mode & 07777U) ? 0 : -EIO;
}

static int64_t sys_symlink_at(uint64_t user_target, int new_dirfd,
                              uint64_t user_link_path) {
    char target[256], link_path[256];
    if (copy_string_from_user(target, sizeof(target), user_target) < 0) return -EFAULT;
    int status = copy_path_at(new_dirfd, user_link_path, link_path);
    if (status != 0) return status;
    if (vfs_lookup_nofollow(link_path)) return -EEXIST;
    int permitted = cred_may_write_parent(link_path);
    if (permitted != 0) return permitted;
    return vfs_create_symlink(link_path, target, 0) ? 0 : -EIO;
}

/* Only the owner and root may change a mode; a non-root owner also loses the
   set-group-ID bit when the file's group is not one of theirs. */
static int64_t change_mode(struct vfs_node *node, uint32_t mode) {
    if (node->flags & VFS_READONLY) return -EROFS;
    const struct credentials *cred = cred_current();
    if (cred && cred->euid != 0 && cred->euid != node->uid) return -EPERM;
    mode &= 07777U;
    if (cred && cred->euid != 0 && !cred_has_group(node->gid)) mode &= ~02000U;
    node->mode = mode;
    vfs_stamp_times(node, VFS_TIME_CTIME);
    vfs_notify_meta_changed(node);
    return 0;
}

/*
 * chown proper. Changing the owner is root-only; the owner may hand the file to
 * one of their own groups. Either way a successful change strips the setuid and
 * setgid bits off an executable, which is what stops a privileged binary from
 * being handed over intact.
 */
static int64_t change_owner(struct vfs_node *node, uint32_t uid, uint32_t gid) {
    if (node->flags & VFS_READONLY) return -EROFS;
    if (uid == CRED_UNCHANGED && gid == CRED_UNCHANGED) return 0;
    const struct credentials *cred = cred_current();
    if (cred && cred->euid != 0) {
        if (uid != CRED_UNCHANGED && uid != node->uid) return -EPERM;
        if (gid != CRED_UNCHANGED) {
            if (cred->euid != node->uid) return -EPERM;
            if (!cred_has_group(gid)) return -EPERM;
        }
    }
    if (uid != CRED_UNCHANGED) node->uid = uid;
    if (gid != CRED_UNCHANGED) node->gid = gid;
    if (node->mode & 0111U) node->mode &= ~06000U;
    vfs_stamp_times(node, VFS_TIME_CTIME);
    vfs_notify_meta_changed(node);
    return 0;
}

static int64_t sys_chmod_at(int dirfd, uint64_t user_path, uint32_t mode, int flags) {
    if (flags & ~AT_SYMLINK_NOFOLLOW) return -EINVAL;
    char path[256];
    int status = copy_path_at(dirfd, user_path, path);
    if (status != 0) return status;
    struct vfs_node *node = (flags & AT_SYMLINK_NOFOLLOW) ? vfs_lookup_nofollow(path) : vfs_lookup(path);
    if (!node) return -ENOENT;
    int permitted = cred_may_search(path);
    if (permitted != 0) return permitted;
    return change_mode(node, mode);
}

static int64_t sys_chown_at(int dirfd, uint64_t user_path, uint32_t uid,
                            uint32_t gid, int flags) {
    if (flags & ~AT_SYMLINK_NOFOLLOW) return -EINVAL;
    char path[256];
    int status = copy_path_at(dirfd, user_path, path);
    if (status != 0) return status;
    struct vfs_node *node = (flags & AT_SYMLINK_NOFOLLOW) ? vfs_lookup_nofollow(path) : vfs_lookup(path);
    if (!node) return -ENOENT;
    int permitted = cred_may_search(path);
    if (permitted != 0) return permitted;
    return change_owner(node, uid, gid);
}

static int64_t sys_fchown(int fd, uint32_t uid, uint32_t gid) {
    struct process *process = process_current();
    if (!process || fd < 0 || fd >= PROCESS_MAX_FDS || !process->files->fds[fd]) return -EBADF;
    struct file *file = process->files->fds[fd];
    if (file->kind != FILE_KIND_VFS || !file->node) return 0;
    return change_owner(file->node, uid, gid);
}

static int64_t sys_fchmod(int fd, uint32_t mode) {
    struct process *process = process_current();
    if (!process || fd < 0 || fd >= PROCESS_MAX_FDS || !process->files->fds[fd]) return -EBADF;
    struct file *file = process->files->fds[fd];
    if (file->kind != FILE_KIND_VFS || !file->node) return -EBADF;
    return change_mode(file->node, mode);
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
        if (!process || dirfd < 0 || dirfd >= PROCESS_MAX_FDS || !process->files->fds[dirfd])
            return -EBADF;
        struct file *file = process->files->fds[dirfd];
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
    /* Setting explicit times needs ownership; "set both to now" only needs
       write permission. */
    if (!cred_owns(node) && (user_times || cred_may(node, CRED_WRITE) != 0))
        return -EPERM;

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

/*
 * Map a memfd's pages into the address space. Unlike map_zero_pages this maps
 * pages that already exist and belong to someone else, taking a reference on
 * each: that is what makes the mapping *shared* rather than a copy, so a write
 * here is visible to every other process that mapped the same object.
 *
 * PAGE_SHARED keeps fork from turning them into copy-on-write pages.
 */
/*
 * Map a memfd's pages into a process.
 *
 * MAP_SHARED maps them writable and marked shared, which is the whole point of
 * a memfd: two processes on the same physical pages. MAP_PRIVATE maps the same
 * pages read-only and copy-on-write instead, so a reader sees the contents
 * without a copy and only pays for one if it writes. Weston's keymap goes out
 * to clients that way -- it reads a sealed memfd with MAP_PRIVATE and never
 * writes to it.
 */
static int map_shared_object(struct process *process, uint64_t start,
                             uint64_t end, struct memfd_object *object,
                             uint64_t file_offset, uint64_t flags, int private) {
    uint64_t extra = PAGE_SHARED;
    if (private) {
        /* Read-only either way; PAGE_COW only when the mapping is writable, so
           that a write to a PROT_READ mapping still faults as it should. */
        extra = (flags & PAGE_WRITE) ? PAGE_COW : 0;
        flags &= ~PAGE_WRITE;
    }
    for (uint64_t address = start; address < end; address += 4096) {
        uint64_t index = (file_offset + (address - start)) / 4096ULL;
        uint64_t physical = memfd_page(object, index);
        /* Past the end of the object: Linux would fault with SIGBUS on access.
           Leaving the page unmapped gets the same observable behaviour. */
        if (!physical) continue;
        if (vmm_translate(process->cr3, address, NULL, NULL) == 0) continue;
        if (pmm_page_ref(physical) != 0) return -1;
        if (vmm_map_page_in(process->cr3, address, physical,
                            flags | extra | PAGE_USER | PAGE_PRESENT) != 0) {
            pmm_free_page((void *)physical);
            return -1;
        }
    }
    return 0;
}

static void unmap_pages(struct process *process, uint64_t start, uint64_t end) {
    /* Forget any mapping record here first: the table must never describe
       pages that are no longer mapped. */
    process_unmap_area(start, end);
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

/*
 * Both questions have to be asked. The map knows about ranges that are owned
 * but not yet resident, which the page tables cannot show; the page tables know
 * about everything mapped by a path that predates the map -- the ELF image, the
 * initial stack, device mappings. Asking the map first is what makes this cheap:
 * an occupied range is rejected without touching a single page table.
 */
static int mapping_range_free(struct process *process, uint64_t base, uint64_t length) {
    if (!process || !length || base >= USER_ADDRESS_LIMIT ||
        length > USER_ADDRESS_LIMIT - base) return 0;
    if (!process_area_range_free(base, base + length)) return 0;
    for (uint64_t page = base; page < base + length; page += 4096) {
        if (vmm_translate(process->cr3, page, NULL, NULL) == 0) return 0;
    }
    return 1;
}

/* The map proposes a gap; mapping_range_free() confirms nothing older is
   sitting in it, and the search moves past the gap if something is. */
static int find_mapping_range(struct process *process, uint64_t start,
                              uint64_t length, uint64_t *base_out) {
    uint64_t base = align_up(start, 4096);
    while (base < USER_ADDRESS_LIMIT && length <= USER_ADDRESS_LIMIT - base) {
        uint64_t candidate;
        if (process_find_free_range(base, length, &candidate) != 0) return -1;
        if (mapping_range_free(process, candidate, length)) {
            *base_out = candidate;
            return 0;
        }
        if (USER_ADDRESS_LIMIT - candidate < length + 4096ULL) break;
        base = candidate + 4096;
    }
    return -1;
}

/* Private pages for [start, end) of a file mapping: the bytes the file still
   has are copied in, everything past its end reads as zero. */
static int copy_file_tail(struct process *process, struct file *file,
                          uint64_t base, uint64_t start, uint64_t end,
                          uint64_t offset, uint64_t page_flags, int prot) {
    if (map_zero_pages(process, base + start, base + end,
                       page_flags | PAGE_WRITE) != 0) return -1;
    uint8_t buffer[256];
    uint64_t copied = start;
    while (copied < end) {
        size_t chunk = end - copied > sizeof(buffer) ? sizeof(buffer)
                                                     : (size_t)(end - copied);
        int64_t amount = vfs_read(file->node, offset + copied, chunk, buffer);
        if (amount <= 0) break;
        if (vmm_copy_to_space(process->cr3, base + copied, buffer,
                              (size_t)amount) != 0) return -1;
        copied += (uint64_t)amount;
        if ((size_t)amount < chunk) break;
    }
    if (!(prot & PROT_WRITE)) {
        uint64_t final_flags = PAGE_USER | PAGE_PRESENT | page_flags;
        for (uint64_t page = base + start; page < base + end; page += 4096) {
            if (vmm_protect_page_in(process->cr3, page, final_flags) != 0) return -1;
        }
    }
    return 0;
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
        if (fd < 0 || fd >= PROCESS_MAX_FDS || !process->files->fds[fd]) return -EBADF;
        file = process->files->fds[fd];
        /* A shared memfd mapping is the one case where two processes really do
           end up on the same physical pages, so it bypasses the copy-the-file
           path below entirely. */
        if (file->kind == FILE_KIND_MEMFD) {
            if (map_shared_object(process, base, base + length, file->memfd,
                                  offset, page_flags,
                                  (flags & MAP_PRIVATE) != 0) != 0) {
                unmap_pages(process, base, base + length);
                return -ENOMEM;
            }
            /* Remember what backs this range so mremap() can grow it later.
               Failing to record only costs the ability to grow, so the mapping
               itself still stands. */
            (void)process_map_area(base, base + length, page_flags, 0,
                                   file, offset);
            if (advance_mmap_base) {
                process->mmap_base = base + length + 4096;
                if (process->memory) process->memory->mmap_base = process->mmap_base;
            }
            return (int64_t)base;
        }
        /* A PRIME descriptor is the buffer itself, so it maps without a node. */
        if (file->kind == FILE_KIND_DMABUF) {
            if (!(flags & MAP_SHARED)) return -EINVAL;
            int64_t status = drm_dmabuf_mmap(file, process->cr3, base, length,
                                             offset, page_flags | PAGE_SHARED);
            if (status < 0) return status;
            if (advance_mmap_base) {
                process->mmap_base = base + length + 4096;
                if (process->memory) process->memory->mmap_base = process->mmap_base;
            }
            return (int64_t)base;
        }
        if ((file->kind != FILE_KIND_VFS && file->kind != FILE_KIND_FRAMEBUFFER) ||
            !file->node) return -ENODEV;
        if (file->node->mmap) {
            if (!(flags & MAP_SHARED)) return -EINVAL;
            /*
             * A device mapping is the device's memory, not the process's, so it
             * must survive fork as the same memory: PAGE_SHARED stops the clone
             * turning it copy-on-write. Weston forks to launch its shell
             * clients while holding its DRM dumb buffers mapped -- without
             * this, every frame it drew afterwards went into a private copy and
             * the screen stayed on whatever was there before.
             */
            int64_t status = file->node->mmap(file->node, file, process->cr3,
                                              base, length, offset,
                                              page_flags | PAGE_SHARED);
            if (status < 0) return status;
            if (advance_mmap_base) {
                process->mmap_base = base + length + 4096;
                if (process->memory) process->memory->mmap_base = process->mmap_base;
            }
            return (int64_t)base;
        }
    }

    /*
     * Private anonymous memory is handed out as address space and paid for a
     * page at a time as it is touched. JSC reserves 8 GiB of cage up front and
     * the webkit web process asks for 84 MiB of arena it barely uses;
     * committing either eagerly is what used to run the machine out of memory.
     */
    if (!file && !(flags & MAP_SHARED)) {
        unmap_pages(process, base, base + length);
        if (process_map_area(base, base + length,
                             page_flags | PAGE_USER | PAGE_PRESENT,
                             VM_ANONYMOUS, NULL, 0) != 0)
            return -ENOMEM;
        if (advance_mmap_base) {
            process->mmap_base = base + length + 4096;
            if (process->memory) process->memory->mmap_base = process->mmap_base;
        }
        return (int64_t)base;
    }

    /*
     * A read-only private file mapping is how every shared library is loaded,
     * and copying it per process is what put webkit over the memory ceiling:
     * libwebkit2gtk alone is 68 MiB and each of its processes wanted its own.
     * Map the kernel's cached copy instead, taking a reference on each page and
     * handing it over copy-on-write so a write still gets a private page.
     *
     * Only whole pages that lie inside the file can be shared. The page the
     * file ends in has to read as zeros past that point -- the loader relies on
     * it for the bss that overlaps the last page -- so it and everything after
     * it get private pages, filled from the file and zero beyond.
     */
    /*
     * MAP_SHARED is the same mapping without the copy-on-write: the pages are
     * the kernel's cached copy of the file, so a write through them is a write
     * every reader sees, which is what MAP_SHARED means. Before this it fell
     * through to the generic path, got private copies, and discarded every
     * write silently -- the kind of failure that loses data instead of
     * reporting anything. A writable shared mapping needs a descriptor that
     * was opened for writing, exactly as Linux requires.
     *
     * Durability is a separate question: the write lands in the cache, and
     * reaching the disk still takes an fsync on the descriptor.
     */
    int share_private = (flags & MAP_PRIVATE) && !(prot & PROT_WRITE);
    int share_shared = (flags & MAP_SHARED) &&
                       (!(prot & PROT_WRITE) ||
                        (file && (file->flags & O_ACCMODE) != O_RDONLY));
    if (file && file->kind == FILE_KIND_VFS && file->node &&
        (file->node->flags & 0xFFU) == VFS_FILE && (share_private || share_shared) &&
        file->node->length <= SHARED_MAP_MAX_BYTES &&
        offset < file->node->length && vfs_fault_in(file->node) == 0 &&
        file->node->data && vfs_align_data(file->node) == 0) {
        uint64_t shareable = (file->node->length - offset) & ~0xFFFULL;
        if (shareable > length) shareable = length;
        uint64_t shared_flags = PAGE_USER | PAGE_PRESENT | PAGE_FILEBACKED;
        if (flags & MAP_SHARED) {
            shared_flags |= PAGE_SHARED;
            if (prot & PROT_WRITE) shared_flags |= PAGE_WRITE;
        } else {
            shared_flags |= PAGE_COW;
        }
        if (nx_enabled && !(prot & PROT_EXEC)) shared_flags |= PAGE_NX;

        uint64_t mapped = 0;
        while (mapped < shareable) {
            uint64_t physical;
            if (vmm_translate(vmm_kernel_cr3(),
                              (uint64_t)file->node->data + offset + mapped,
                              &physical, NULL) != 0) break;
            physical &= ~0xFFFULL;
            /* The heap's own hold on this frame is not counted, so claim it
               here too: that reference is what keeps the cached copy alive
               once the last mapping of it goes away. */
            if (pmm_page_refcount(physical) == 0 && pmm_page_ref(physical) != 0) break;
            if (pmm_page_ref(physical) != 0) break;
            if (vmm_map_page_in(process->cr3, base + mapped, physical,
                                shared_flags) != 0) {
                pmm_free_page((void *)physical);
                break;
            }
            mapped += 4096;
        }

        if (mapped == shareable) {
            /* Recorded like any other mapping, which is what holds the node's
               contents: the area's VM_FILE_PAGES reference is dropped when the
               range is unmapped or the process goes, and the cache becomes
               reclaimable again once the last one has left. */
            if (process_map_area(base, base + length, page_flags, VM_FILE_PAGES,
                                 file, offset) != 0) {
                unmap_pages(process, base, base + mapped);
                return -ENOMEM;
            }
            if (mapped < length &&
                copy_file_tail(process, file, base, mapped, length, offset,
                               page_flags, prot) != 0) {
                unmap_pages(process, base, base + length);
                return -ENOMEM;
            }
            if (advance_mmap_base) {
                process->mmap_base = base + length + 4096;
                if (process->memory) process->memory->mmap_base = process->mmap_base;
            }
            return (int64_t)base;
        }
        /* Partial run: drop it and copy the whole mapping the ordinary way. */
        unmap_pages(process, base, base + mapped);
    }

    uint64_t allocation_flags = file ? (page_flags | PAGE_WRITE) : page_flags;
    /* MAP_SHARED|MAP_ANONYMOUS has to survive fork as shared memory rather than
       being copied per process, which is what PAGE_SHARED tells the clone to
       do. Without a file behind it there is nothing to share with beyond the
       processes that inherit the mapping. */
    if ((flags & MAP_SHARED) && (flags & MAP_ANONYMOUS)) allocation_flags |= PAGE_SHARED;
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
        /* The process has its own copy now; the disk can hold the other one.
           Without this every mapped library is resident twice. */
        if (file->kind == FILE_KIND_VFS) vfs_release_data(file->node);
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

static int64_t sys_shmget(int32_t key, uint64_t size, int flags) {
    return sysvshm_get(key, size, flags, (uint32_t)process_current_pid());
}

/*
 * shmat(2). The segment's pages are mapped exactly the way a shared memfd is,
 * and the area records the segment's file so that the reference counting which
 * already survives fork and exit is what decides when the memory goes away.
 */
static int64_t sys_shmat(int id, uint64_t address, int flags) {
    struct process *process = process_current();
    if (!process) return -EINVAL;

    uint64_t size = 0;
    struct file *file = sysvshm_acquire(id, &size);
    if (!file) return -EINVAL;
    uint64_t length = align_up(size, 4096);
    if (!length) { file_unref(file); return -EINVAL; }

    uint64_t base;
    int advance_mmap_base = 0;
    if (address) {
        if (address & 0xFFFULL) {
            if (!(flags & SHM_RND)) { file_unref(file); return -EINVAL; }
            address &= ~0xFFFULL;
        }
        base = address;
    } else {
        uint64_t start = process->memory ? process->memory->mmap_base : process->mmap_base;
        if (find_mapping_range(process, start, length, &base) != 0) {
            file_unref(file);
            return -ENOMEM;
        }
        advance_mmap_base = 1;
    }
    if (base < 0x10000ULL || base >= USER_ADDRESS_LIMIT ||
        length > USER_ADDRESS_LIMIT - base) {
        file_unref(file);
        return -EINVAL;
    }

    uint64_t page_flags = (flags & SHM_RDONLY) ? 0 : PAGE_WRITE;
    if (nx_enabled) page_flags |= PAGE_NX;
    if (map_shared_object(process, base, base + length, file->memfd, 0,
                          page_flags, 0) != 0) {
        unmap_pages(process, base, base + length);
        file_unref(file);
        return -ENOMEM;
    }
    if (process_map_area(base, base + length, page_flags, 0, file, 0) != 0) {
        unmap_pages(process, base, base + length);
        file_unref(file);
        return -ENOMEM;
    }
    /* process_map_area took its own reference; this one has done its job. */
    file_unref(file);

    if (advance_mmap_base) {
        process->mmap_base = base + length + 4096;
        if (process->memory) process->memory->mmap_base = process->mmap_base;
    }
    sysvshm_touch(id, (uint32_t)process_current_pid(), 1);
    return (int64_t)base;
}

static int64_t sys_shmdt(uint64_t address) {
    struct process *process = process_current();
    if (!process || (address & 0xFFFULL)) return -EINVAL;
    struct vm_area *area = process_find_area(address);
    if (!area || area->start != address) return -EINVAL;
    unmap_pages(process, area->start, area->end);
    /* That may have been the last attachment to a segment already removed. */
    sysvshm_reap();
    return 0;
}

static int64_t sys_shmctl(int id, int command, uint64_t user_buffer) {
    /* IPC_64 selects the 64-bit structures, which are the only ones here. */
    command &= ~IPC_64;
    switch (command) {
        case IPC_RMID:
            return sysvshm_remove(id);
        case IPC_STAT: {
            struct shm_id_ds value;
            int status = sysvshm_stat(id, &value);
            if (status != 0) return status;
            if (!user_buffer) return -EFAULT;
            if (copy_to_user(user_buffer, &value, sizeof(value)) != 0) return -EFAULT;
            return 0;
        }
        case IPC_SET: {
            struct shm_id_ds value;
            if (!user_buffer) return -EFAULT;
            if (copy_from_user(&value, user_buffer, sizeof(value)) != 0) return -EFAULT;
            return sysvshm_set(id, value.mode, value.uid, value.gid);
        }
        default:
            return -EINVAL;
    }
}

/*
 * mremap(2), enough of it for a growing shm pool.
 *
 * libwayland's wl_shm resizes a client's pool by mremap()ing the server's
 * mapping of it, with no fallback: without this the compositor kills the client
 * with a "failed mremap" protocol error.
 *
 * Growing a *file* mapping means covering more of the backing object, which
 * page tables alone cannot describe -- hence the mapping table in
 * process_memory. Growing an *anonymous* mapping just needs fresh zero pages,
 * so that case works without a record, but only in place: moving anonymous
 * pages is not implemented and returns ENOMEM rather than quietly handing back
 * a mapping with the wrong contents.
 */
/*
 * Growing or shrinking a mapping in place, and moving it when it cannot grow.
 * `backing` is the object behind the mapping, already referenced by the caller:
 * unmap_pages() frees the area it came from, and with it that area's reference,
 * so the pointer would otherwise go stale halfway through.
 */
static int64_t mremap_backed(struct process *process, uint64_t address,
                             uint64_t old_length, uint64_t new_length,
                             int flags, struct file *backing,
                             uint64_t backing_offset, uint32_t kind) {
    if (new_length < old_length) {
        unmap_pages(process, address + new_length, address + old_length);
        /* The kind travels with the record. A mapping that shares the file's
           cached pages holds a claim on them through its area, and rewriting
           the record without it would let the cache be dropped from under a
           mapping that is still there. */
        if (backing)
            (void)process_map_area(address, address + new_length, PAGE_WRITE,
                                   kind, backing, backing_offset);
        return (int64_t)address;
    }

    /* Growing. Try in place first, which keeps every existing pointer valid. */
    uint64_t tail = address + old_length;
    uint64_t extra = new_length - old_length;
    if (mapping_range_free(process, tail, extra)) {
        int ok;
        if (backing && backing->kind == FILE_KIND_MEMFD) {
            ok = map_shared_object(process, tail, tail + extra, backing->memfd,
                                   backing_offset + old_length,
                                   PAGE_WRITE, 0) == 0;
        } else if (!backing) {
            ok = map_zero_pages(process, tail, tail + extra, PAGE_WRITE) == 0;
        } else {
            ok = 0; /* a file mapping we cannot extend */
        }
        if (ok) {
            if (backing)
                (void)process_map_area(address, address + new_length, PAGE_WRITE,
                                       kind, backing, backing_offset);
            return (int64_t)address;
        }
        unmap_pages(process, tail, tail + extra);
    }

    if (!(flags & MREMAP_MAYMOVE)) return -ENOMEM;
    /* Moving is only possible when we know what backs the mapping, because the
       new location is populated from the object rather than by copying pages. */
    if (!backing || backing->kind != FILE_KIND_MEMFD) return -ENOMEM;

    uint64_t destination;
    uint64_t search_start = process->memory ? process->memory->mmap_base :
                                              process->mmap_base;
    if (find_mapping_range(process, search_start, new_length, &destination) != 0)
        return -ENOMEM;

    if (map_shared_object(process, destination, destination + new_length,
                          backing->memfd, backing_offset, PAGE_WRITE, 0) != 0) {
        unmap_pages(process, destination, destination + new_length);
        return -ENOMEM;
    }

    /* The old range's pages are the object's, and the new mapping took its own
       references, so release the old ones normally. */
    unmap_pages(process, address, address + old_length);
    (void)process_map_area(destination, destination + new_length, PAGE_WRITE,
                           kind, backing, backing_offset);

    process->mmap_base = destination + new_length + 4096;
    if (process->memory) process->memory->mmap_base = process->mmap_base;
    return (int64_t)destination;
}

static int64_t sys_mremap(uint64_t address, uint64_t old_length,
                          uint64_t new_length, int flags, uint64_t new_address) {
    struct process *process = process_current();
    if (!process) return -EINVAL;
    if (address & 0xFFFULL) return -EINVAL;
    if (!new_length) return -EINVAL;
    /* MREMAP_FIXED, which is the only use for new_address, is not supported. */
    if (flags & ~MREMAP_MAYMOVE) return -EINVAL;
    (void)new_address;

    old_length = align_up(old_length, 4096);
    new_length = align_up(new_length, 4096);
    if (address >= USER_ADDRESS_LIMIT ||
        new_length > USER_ADDRESS_LIMIT - address) return -EINVAL;
    if (vmm_translate(process->cr3, address, NULL, NULL) != 0) return -EFAULT;

    if (new_length == old_length) return (int64_t)address;

    struct vm_area *area = process_find_area(address);
    struct file *backing = area ? area->file : NULL;
    uint64_t backing_offset = area ? area->offset : 0;
    uint32_t kind = area ? area->kind : 0;
    /* Held across the whole operation: the area this came from is freed by the
       first unmap, and its reference goes with it. */
    if (backing) file_ref(backing);
    int64_t result = mremap_backed(process, address, old_length, new_length,
                                   flags, backing, backing_offset, kind);
    if (backing) file_unref(backing);
    return result;
}

static int64_t sys_mprotect(uint64_t address, uint64_t length, int prot) {
    struct process *process = process_current();
    if (!process || (address & 0xFFFULL) || !length) return -EINVAL;
    length = align_up(length, 4096);
    uint64_t flags = PAGE_USER | PAGE_PRESENT;
    if (prot & PROT_WRITE) flags |= PAGE_WRITE;
    if (nx_enabled && !(prot & PROT_EXEC)) flags |= PAGE_NX;
    /* Uncommitted pages have no entry to protect; record the new flags on the
       map so they get them when a fault commits them. */
    process_protect_area(address, address + length, flags | PAGE_PRESENT);
    int reserved = !process_area_range_free(address, address + length);
    for (uint64_t page = address; page < address + length; page += 4096) {
        uint64_t old_flags;
        if (vmm_translate(process->cr3, page, NULL, &old_flags) != 0) {
            if (reserved) continue;
            return -ENOMEM;
        }
        uint64_t effective_flags = flags | (old_flags & (PAGE_DEVICE | PAGE_SHARED));
        if (nx_enabled && (old_flags & PAGE_DEVICE)) effective_flags |= PAGE_NX;
        /*
         * A copy-on-write page must not be handed write permission here: it is
         * still shared with whoever forked it, and granting the write directly
         * would let the two processes scribble on each other's memory. Keep it
         * read-only and copy-on-write so the next store faults and gets a
         * private copy, which is the permission the caller actually asked for.
         *
         * Dropping write permission instead clears the marker, so that a later
         * store gets the SIGSEGV the caller asked for rather than a silent copy.
         */
        if (old_flags & PAGE_COW) {
            if (prot & PROT_WRITE) effective_flags = (effective_flags & ~PAGE_WRITE) | PAGE_COW;
        }
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

static int parse_shebang(struct vfs_node *file, char interpreter[MAX_SHEBANG_LINE],
                          char optional_argument[MAX_SHEBANG_LINE]) {
    unsigned char header[MAX_SHEBANG_LINE];
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
    if (!length || length >= MAX_SHEBANG_LINE || header[start] != '/') return -ENOEXEC;
    memcpy(interpreter, header + start, length);
    interpreter[length] = '\0';

    while (at < (size_t)read && (header[at] == ' ' || header[at] == '\t')) at++;
    start = at;
    while (at < (size_t)read && header[at] != '\n' && header[at] != '\r') at++;
    while (at > start && (header[at - 1] == ' ' || header[at - 1] == '\t')) at--;
    length = at - start;
    if (length >= MAX_SHEBANG_LINE) return -ENOEXEC;
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
    int permitted = cred_may_path(path, file, CRED_EXEC);
    if (permitted != 0) {
        kfree(arguments);
        return permitted;
    }

    char interpreter[MAX_SHEBANG_LINE];
    char optional_argument[MAX_SHEBANG_LINE];
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
        permitted = cred_may_path(interpreter, interpreter_file, CRED_EXEC);
        if (permitted != 0) {
            kfree(arguments);
            return permitted;
        }
        int rewritten = rewrite_script_arguments(arguments, argc, path, interpreter,
                                                 optional_argument);
        if (rewritten < 0) {
            kfree(arguments);
            return rewritten;
        }
        int64_t result = process_exec_from_syscall(frame, interpreter,
                                                   arguments->argv, arguments->envp, NULL);
        kfree(arguments);
        return result == -1 ? -ENOEXEC : result;
    }

    int64_t result = process_exec_from_syscall(frame, path, arguments->argv,
                                               arguments->envp, file);
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
        process_set_sigaction(signal_number, &action);
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
    /* The queue belonging to the object when it has one -- a pipe, a terminal
       -- and the general one when it does not, which is every socket.
       Yielding instead is what made a blocking read on a socket a spin. */
    const void *channel = writing ? file_write_wait_channel(file)
                                  : file_read_wait_channel(file);
    if (!channel) channel = process_io_wait_channel();
    if (process_sleep_on(frame, channel) != 0)
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

/*
 * The machine's name, which uname(2) used to answer with a constant. The first
 * thing runit does on the way up is read /etc/hostname and call sethostname(2),
 * so a kernel that cannot be told its own name reports the wrong one to
 * everything that asks for the rest of the boot.
 */
static int64_t set_machine_name(int domain, uint64_t user_name, uint64_t length) {
    const struct credentials *cred = cred_current();
    if (cred && cred->euid != 0) return -EPERM;
    if (length > UTS_NAME_MAX) return -EINVAL;

    char value[UTS_NAME_MAX + 1];
    if (length && copy_from_user(value, user_name, (size_t)length) != 0)
        return -EFAULT;
    value[length] = '\0';
    if (domain) uts_set_domainname(value, (size_t)length);
    else uts_set_hostname(value, (size_t)length);
    return 0;
}

/*
 * sysinfo(2) and times(2), which everything that reports on the machine wants:
 * free(1), uptime(1), top(1) and fastfetch all go through one or the other, and
 * a missing one is not a degraded answer but a tool that prints an error.
 */

#define CLOCK_TICKS_PER_SECOND 100ULL
#define NANOSECONDS_PER_TICK (1000000000ULL / CLOCK_TICKS_PER_SECOND)

struct linux_sysinfo {
    int64_t uptime;
    uint64_t loads[3];
    uint64_t totalram;
    uint64_t freeram;
    uint64_t sharedram;
    uint64_t bufferram;
    uint64_t totalswap;
    uint64_t freeswap;
    uint16_t procs;
    uint16_t pad;
    uint64_t totalhigh;
    uint64_t freehigh;
    uint32_t mem_unit;
    char reserved[4];
};

_Static_assert(sizeof(struct linux_sysinfo) == 112U, "sysinfo ABI size mismatch");

struct linux_tms {
    int64_t tms_utime;
    int64_t tms_stime;
    int64_t tms_cutime;
    int64_t tms_cstime;
};

static int64_t sys_sysinfo(uint64_t user_buffer) {
    struct linux_sysinfo value;
    memset(&value, 0, sizeof(value));
    value.uptime = (int64_t)(time_uptime_ns() / 1000000000ULL);
    /* There is no load average to report: the scheduler keeps no history, and
       inventing one would be worse than the zero every reader already
       tolerates. */
    value.totalram = pmm_usable_page_count() * PMM_PAGE_SIZE;
    value.freeram = pmm_free_page_count() * PMM_PAGE_SIZE;
    value.procs = (uint16_t)process_count();
    value.mem_unit = 1;
    return copy_to_user(user_buffer, &value, sizeof(value)) == 0 ? 0 : -EFAULT;
}

/*
 * All of a process's time is charged as user time. Splitting it would mean
 * timing the syscall path itself, and the scheduler only accounts for how long
 * a process was on a processor at all -- see process_account_runtime().
 */
static int64_t sys_times(uint64_t user_buffer) {
    if (user_buffer) {
        struct linux_tms value;
        memset(&value, 0, sizeof(value));
        struct process *process = process_current();
        if (process)
            value.tms_utime = (int64_t)(process_runtime_ns(process) / NANOSECONDS_PER_TICK);
        if (copy_to_user(user_buffer, &value, sizeof(value)) != 0) return -EFAULT;
    }
    return (int64_t)(time_uptime_ns() / NANOSECONDS_PER_TICK);
}

/* getrusage(2). Only the time is real; the page-fault and context-switch
   counters are not kept anywhere. */
static int64_t sys_getrusage(uint64_t user_buffer) {
    struct { int64_t seconds; int64_t microseconds; } utime = {0, 0};
    uint8_t value[144];
    memset(value, 0, sizeof(value));

    struct process *process = process_current();
    if (process) {
        uint64_t nanoseconds = process_runtime_ns(process);
        utime.seconds = (int64_t)(nanoseconds / 1000000000ULL);
        utime.microseconds = (int64_t)((nanoseconds % 1000000000ULL) / 1000ULL);
    }
    memcpy(value, &utime, sizeof(utime));
    return copy_to_user(user_buffer, value, sizeof(value)) == 0 ? 0 : -EFAULT;
}

static int64_t sys_uname(uint64_t user_buffer) {
    struct linux_utsname value;
    memset(&value, 0, sizeof(value));
    strncpy(value.sysname, "Tunix", sizeof(value.sysname) - 1);
    strncpy(value.nodename, uts_hostname(), sizeof(value.nodename) - 1);
    strncpy(value.release, "0.1.0", sizeof(value.release) - 1);
    strncpy(value.version, "Tunix Kernel", sizeof(value.version) - 1);
    strncpy(value.machine, "x86_64", sizeof(value.machine) - 1);
    strncpy(value.domainname, uts_domainname(), sizeof(value.domainname) - 1);
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

/*
 * The set of processors a task may run on, which is all of them.
 *
 * nproc, and every thread pool that sizes itself, asks this before it looks at
 * /proc/cpuinfo -- so a kernel that answers ENOSYS here reports one processor
 * however many it is running. The return value is the number of bytes written,
 * not zero, which is the part callers actually read.
 */
static int64_t sys_sched_getaffinity(uint64_t tid, size_t size, uint64_t user_mask) {
    if (!user_mask) return -EFAULT;
    if (size < sizeof(uint64_t) || (size & (sizeof(uint64_t) - 1))) return -EINVAL;

    uint64_t mask = 0;
    int result = process_get_affinity(tid, &mask);
    if (result != 0) return result;

    size_t bytes = size > sizeof(uint64_t) ? sizeof(uint64_t) : size;
    if (copy_to_user(user_mask, &mask, bytes) != 0) return -EFAULT;
    for (size_t offset = sizeof(uint64_t); offset < size; offset += sizeof(uint64_t)) {
        uint64_t zero = 0;
        if (copy_to_user(user_mask + offset, &zero, sizeof(zero)) != 0) return -EFAULT;
    }
    return (int64_t)size;
}

static int64_t sys_sched_setaffinity(uint64_t tid, size_t size, uint64_t user_mask) {
    if (!user_mask) return -EFAULT;
    if (size < sizeof(uint64_t) || (size & (sizeof(uint64_t) - 1))) return -EINVAL;
    uint64_t mask = 0;
    if (copy_from_user(&mask, user_mask, sizeof(mask)) != 0) return -EFAULT;
    return process_set_affinity(tid, mask);
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

/* Linux capabilities, uapi/linux/capability.h. This kernel's privilege model
 * (cred.h: "the POSIX model, without capabilities") never tracks capability
 * bits separately from euid, so there is nothing to store -- capget()
 * synthesizes its answer from euid, and capset() only has to agree with
 * whatever capget() would say. The version-negotiation dance (report our
 * version back and fail once when the caller's header names an older one)
 * mirrors what real Linux does, because libcap always tries it first. */
#define LINUX_CAPABILITY_VERSION_3 0x20080522U

struct cap_user_header {
    uint32_t version;
    int32_t pid;
};

struct cap_user_data {
    uint32_t effective;
    uint32_t permitted;
    uint32_t inheritable;
};

static int64_t sys_capget(uint64_t user_header, uint64_t user_data) {
    if (!user_header) return -EFAULT;
    struct cap_user_header header;
    if (copy_from_user(&header, user_header, sizeof(header)) != 0) return -EFAULT;

    if (header.version != LINUX_CAPABILITY_VERSION_3) {
        header.version = LINUX_CAPABILITY_VERSION_3;
        if (copy_to_user(user_header, &header, sizeof(header)) != 0) return -EFAULT;
        return user_data ? -EINVAL : 0;
    }
    if (!user_data) return 0;
    if (header.pid < 0) return -EINVAL;

    struct process *target = header.pid ? process_find((uint64_t)header.pid) : process_current();
    if (!target) return -ESRCH;

    uint32_t bits = target->cred.euid == 0 ? 0xFFFFFFFFU : 0U;
    struct cap_user_data data[2];
    data[0].effective = data[0].permitted = data[0].inheritable = bits;
    data[1].effective = data[1].permitted = data[1].inheritable = bits;
    return copy_to_user(user_data, data, sizeof(data)) == 0 ? 0 : -EFAULT;
}

static int64_t sys_capset(uint64_t user_header, uint64_t user_data) {
    if (!user_header) return -EFAULT;
    struct cap_user_header header;
    if (copy_from_user(&header, user_header, sizeof(header)) != 0) return -EFAULT;

    if (header.version != LINUX_CAPABILITY_VERSION_3) {
        header.version = LINUX_CAPABILITY_VERSION_3;
        (void)copy_to_user(user_header, &header, sizeof(header));
        return -EINVAL;
    }
    struct process *process = process_current();
    if (header.pid && (!process || (uint64_t)header.pid != process->pid)) return -EPERM;
    if (!user_data) return 0;

    /* A process cannot hold a capability its euid does not already grant it
     * in this model, so the only real question is whether the caller is
     * trying to claim one it lacks -- which is exactly what capget() above
     * would have reported as an empty set. */
    return cred_is_root() ? 0 : -EPERM;
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
            return process->keep_capabilities;
        case PR_SET_KEEPCAPS:
            if (arg2 > 1) return -EINVAL;
            /*
             * Keep the permitted capability set across a change of uid. There
             * are no capabilities here, so there is nothing to keep -- but the
             * flag is a promise about a later setuid rather than an action,
             * and refusing it is not the same as having nothing to do. It is
             * the first thing iputils' ping asks for, and being told EINVAL
             * made it exit with "ping: prctl: Invalid argument" before it
             * opened a socket.
             */
            process->keep_capabilities = (int)arg2;
            return 0;
        case PR_GET_SECUREBITS:
        case PR_GET_SECCOMP:
            return 0;
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

#define RLIMIT_NOFILE 7
static int64_t sys_prlimit(uint64_t resource, uint64_t user_old_limit) {
    if (!user_old_limit) return 0;
    /* Report the real fd ceiling for NOFILE: an infinite value makes musl's
     * sysconf(_SC_OPEN_MAX) return -1, which trips Xtrans's fd-range guard and
     * blocks the X server from opening its listening sockets. */
    uint64_t lim = (resource == RLIMIT_NOFILE) ? PROCESS_MAX_FDS : UINT64_MAX;
    struct linux_rlimit value = {lim, lim};
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

    /*
     * posix_spawn: CLONE_VM|CLONE_VFORK plus a stack for the helper that
     * execs. The child gets a copy of the address space rather than sharing
     * it, which is enough because the helper reports its result down a pipe,
     * and the parent runs on rather than blocking -- it is already waiting on
     * that pipe. GLib spawns every WebKit child process this way.
     */
    if ((flags & CLONE_VFORK) && (flags & CLONE_VM) && child_stack) {
        uint64_t vfork_allowed = CLONE_VM | CLONE_VFORK | CLONE_FS | CLONE_FILES |
                                 CLONE_SIGHAND | CLONE_SYSVSEM | CLONE_SETTLS |
                                 CLONE_FORK_METADATA_FLAGS;
        if (flags & ~(0xFFULL | vfork_allowed)) return -ENOSYS;
        if (exit_signal != 0 && exit_signal != SIGCHLD) return -EINVAL;
        int64_t vfork_pid = process_fork_from_syscall(frame);
        if (vfork_pid <= 0) return vfork_pid;
        struct process *spawned = process_find((uint64_t)vfork_pid);
        if (!spawned) return -ESRCH;
        spawned->saved_frame.user_rsp = child_stack;
        return vfork_pid;
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
    return process->files->fds[fd];
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

/*
 * memfd_create(2). The name argument only shows up in /proc on Linux and is
 * ignored here beyond validating that it is readable.
 *
 * MFD_ALLOW_SEALING used to be accepted and ignored, on the reasoning that a
 * client asking for sealing still works without it. That is true of libwayland
 * and false of Mesa, which asks for F_SEAL_SHRINK before handing a buffer to a
 * compositor and, when the request fails, closes the descriptor and gives up.
 * The request then went out with no descriptor attached and the compositor
 * answered `invalid arguments for wl_shm.create_pool` -- which is how every
 * OpenGL client on this system failed to open a window.
 */
static int64_t sys_memfd_create(uint64_t user_name, uint32_t flags) {
    if (flags & ~(uint32_t)(MFD_CLOEXEC | MFD_ALLOW_SEALING)) return -EINVAL;
    char name[256];
    if (copy_string_from_user(name, sizeof(name), user_name) < 0) return -EFAULT;

    struct memfd_object *object = memfd_create_object();
    if (!object) return -ENOMEM;
    if (flags & MFD_ALLOW_SEALING) memfd_allow_sealing(object);
    struct file *file = file_create_memfd(object, 0);
    if (!file) {
        memfd_destroy(object);
        return -ENOMEM;
    }
    return install_new_file(file, flags & MFD_CLOEXEC);
}

/*
 * signalfd4(2). `fd` of -1 creates a descriptor; anything else re-arms the mask
 * on an existing one, which is how a program narrows or widens what it watches
 * without churning its epoll set.
 *
 * The caller is expected to have blocked these signals with sigprocmask first,
 * exactly as on Linux -- otherwise the normal delivery path consumes them
 * before a read ever happens. That is the caller's job, not ours.
 */
static int64_t sys_signalfd(int fd, uint64_t user_mask, uint64_t mask_size,
                            int flags) {
    struct process *process = process_current();
    if (!process) return -EINVAL;
    if (flags & ~(SFD_NONBLOCK | SFD_CLOEXEC)) return -EINVAL;
    if (mask_size != sizeof(uint64_t)) return -EINVAL;

    uint64_t mask = 0;
    if (copy_from_user(&mask, user_mask, sizeof(mask)) != 0) return -EFAULT;

    if (fd >= 0) {
        if (fd >= PROCESS_MAX_FDS || !process->files->fds[fd]) return -EBADF;
        struct file *file = process->files->fds[fd];
        if (file->kind != FILE_KIND_SIGNALFD) return -EINVAL;
        signalfd_set_mask(file->signalfd, mask);
        return fd;
    }

    struct signalfd_context *context = signalfd_create(mask);
    if (!context) return -ENOMEM;
    struct file *file = file_create_signalfd(context,
        (uint32_t)(flags & SFD_NONBLOCK));
    if (!file) {
        signalfd_destroy(context);
        return -ENOMEM;
    }
    return install_new_file(file, flags & SFD_CLOEXEC);
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
    if (!user_events || maximum <= 0) return -EINVAL;
    /* Clamp to our on-stack batch rather than rejecting: epoll_wait may return
     * fewer than maxevents, and the caller loops.  Xorg's ospoll asks for 256. */
    if (maximum > 128) maximum = 128;
    struct file *file = file_from_fd(epoll_fd);
    if (!file || file->kind != FILE_KIND_EPOLL) return -EBADF;
    struct tunix_epoll_event events[128];
    int ready = epoll_collect(file->epoll, events, maximum, 0);
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

/*
 * How much of the machine the file cache may hold.
 *
 * A sixteenth of RAM, floored and capped so neither a small machine nor a large
 * one gets a silly answer. The number that used to bound it was the heap's own
 * ceiling -- three quarters of 2 GiB -- which on any machine with room to spare
 * meant the cache simply grew until something else could not allocate. Reading
 * a few hundred megabytes of files is an ordinary morning's work, and every
 * byte of it stayed resident for the life of the boot.
 */
static uint64_t file_cache_budget(void) {
    static uint64_t budget;
    if (budget) return budget;
    budget = pmm_usable_page_count() * PMM_PAGE_SIZE / 16ULL;
    if (budget < 32ULL * 1024 * 1024) budget = 32ULL * 1024 * 1024;
    if (budget > 128ULL * 1024 * 1024) budget = 128ULL * 1024 * 1024;
    return budget;
}

/*
 * reboot(2).
 *
 * The magic numbers are not decoration: the call takes the machine away, and
 * requiring two constants and a command means a wild syscall with plausible
 * arguments cannot reach it by accident. Linux accepts four values for the
 * second one, all of them dates, and libcs differ over which they pass.
 */
#define LINUX_REBOOT_MAGIC1 0xFEE1DEADU
#define LINUX_REBOOT_MAGIC2 0x28121969U
#define LINUX_REBOOT_MAGIC2A 0x05121996U
#define LINUX_REBOOT_MAGIC2B 0x16041998U
#define LINUX_REBOOT_MAGIC2C 0x20112000U

#define LINUX_REBOOT_CMD_RESTART 0x01234567U
#define LINUX_REBOOT_CMD_HALT 0xCDEF0123U
#define LINUX_REBOOT_CMD_POWER_OFF 0x4321FEDCU
#define LINUX_REBOOT_CMD_CAD_ON 0x89ABCDEFU
#define LINUX_REBOOT_CMD_CAD_OFF 0x00000000U

static int64_t sys_reboot(uint32_t magic1, uint32_t magic2, uint32_t command) {
    const struct credentials *cred = cred_current();
    if (cred && cred->euid != 0) return -EPERM;
    if (magic1 != LINUX_REBOOT_MAGIC1) return -EINVAL;
    if (magic2 != LINUX_REBOOT_MAGIC2 && magic2 != LINUX_REBOOT_MAGIC2A &&
        magic2 != LINUX_REBOOT_MAGIC2B && magic2 != LINUX_REBOOT_MAGIC2C)
        return -EINVAL;

    switch (command) {
        /* These return, and are the reason this is not simply noreturn: an init
           that turns the power button over to itself carries on running. */
        case LINUX_REBOOT_CMD_CAD_ON:  power_set_button_handled(1); return 0;
        case LINUX_REBOOT_CMD_CAD_OFF: power_set_button_handled(0); return 0;
        case LINUX_REBOOT_CMD_RESTART:   power_restart();
        case LINUX_REBOOT_CMD_HALT:      power_halt();
        case LINUX_REBOOT_CMD_POWER_OFF: power_off();
        default: return -EINVAL;
    }
}

static void syscall_dispatch_locked(struct syscall_frame *frame) {
    if (!frame) return;
    process_account_runtime();
    process_reap_deferred();
    /* File contents live in the heap. Entering a syscall is the one moment when
       no kernel path is holding a pointer into a file's buffer, so it is the
       only safe place to drop them. The budget is the ordinary bound; the heap
       running out is the emergency behind it, and takes everything. */
    vfs_trim_cache(file_cache_budget());
    if (heap_under_pressure()) vfs_reclaim_file_data(vfs_root);
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
            struct file *file = process && fd >= 0 && fd < PROCESS_MAX_FDS ? process->files->fds[fd] : NULL;
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
        case SYS_MREMAP:
            frame->rax = (uint64_t)sys_mremap(frame->rdi, frame->rsi, frame->rdx,
                                              (int)frame->r10, frame->r8);
            break;
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
        case SYS_SHMGET:
            frame->rax = (uint64_t)sys_shmget((int32_t)frame->rdi, frame->rsi, (int)frame->rdx);
            break;
        case SYS_SHMAT:
            frame->rax = (uint64_t)sys_shmat((int)frame->rdi, frame->rsi, (int)frame->rdx);
            break;
        case SYS_SHMDT: frame->rax = (uint64_t)sys_shmdt(frame->rdi); break;
        case SYS_SHMCTL:
            frame->rax = (uint64_t)sys_shmctl((int)frame->rdi, (int)frame->rsi, frame->rdx);
            break;
        case SYS_BRK: frame->rax = (uint64_t)sys_brk(frame->rdi); break;
        case SYS_RT_SIGACTION: frame->rax = (uint64_t)sys_sigaction((int)frame->rdi, frame->rsi, frame->rdx, frame->r10); break;
        case SYS_RT_SIGPROCMASK: frame->rax = (uint64_t)sys_sigprocmask((int)frame->rdi, frame->rsi, frame->rdx, frame->r10); break;
        case SYS_RT_SIGRETURN:
            if (process_sigreturn(frame) != 0) frame->rax = (uint64_t)-(int64_t)EINVAL;
            skip_signal_delivery = 1;
            break;
        case SYS_IOCTL: {
            int fd = (int)frame->rdi;
            int64_t result = sys_ioctl(fd, (unsigned long)frame->rsi, frame->rdx);
            struct file *file = file_from_fd(fd);
            /*
             * An ioctl that moves data can run out of room, and on a blocking
             * descriptor it has to wait rather than fail -- ALSA fills its ring
             * through SNDRV_PCM_IOCTL_WRITEI_FRAMES, and alsa-lib treats an
             * EAGAIN from it as a hard error. Only devices that publish a
             * write-readiness hook can block this way, which keeps every other
             * ioctl in the tree on the path it had.
             */
            if (result == -EAGAIN && file && !(file->flags & O_NONBLOCK) &&
                file->kind == FILE_KIND_VFS && file->node &&
                file->node->write_ready) {
                block_and_retry(frame, SYS_IOCTL, file, 1);
            } else if (result == -EAGAIN && (unsigned long)frame->rsi == VT_WAITACTIVE) {
                /*
                 * Waiting for a terminal to come to the front. The switch is
                 * held up by a program that was asked to release the display
                 * and has not answered yet, so this sleeps until it does --
                 * which is exactly what a display manager expects of
                 * VT_WAITACTIVE, and what it would otherwise spin on.
                 */
                frame->user_rip -= 2U;
                frame->rax = SYS_IOCTL;
                struct process *waiter = process_current();
                if (waiter) waiter->syscall_rewound = 1;
                if (process_sleep_on(frame, vt_switch_wait_channel()) != 0)
                    process_yield_from_syscall(frame);
            } else {
                frame->rax = (uint64_t)result;
            }
            break;
        }
        case SYS_PREAD64: {
            struct process *process = process_current();
            int fd = (int)frame->rdi;
            if (!process || fd < 0 || fd >= PROCESS_MAX_FDS || !process->files->fds[fd] || process->files->fds[fd]->kind != FILE_KIND_VFS) frame->rax = (uint64_t)-(int64_t)EBADF;
            else {
                uint64_t saved = process->files->fds[fd]->offset;
                process->files->fds[fd]->offset = frame->r10;
                frame->rax = (uint64_t)sys_read(fd, frame->rsi, (size_t)frame->rdx);
                process->files->fds[fd]->offset = saved;
            }
            break;
        }
        case SYS_PWRITE64: {
            struct process *process = process_current();
            int fd = (int)frame->rdi;
            if (!process || fd < 0 || fd >= PROCESS_MAX_FDS || !process->files->fds[fd] || process->files->fds[fd]->kind != FILE_KIND_VFS) frame->rax = (uint64_t)-(int64_t)EBADF;
            else {
                uint64_t saved = process->files->fds[fd]->offset;
                process->files->fds[fd]->offset = frame->r10;
                frame->rax = (uint64_t)sys_write(fd, frame->rsi, (size_t)frame->rdx);
                process->files->fds[fd]->offset = saved;
            }
            break;
        }
        /* preadv/pwritev, as pread/pwrite are done a few lines above: move the
           offset, run the vectored call, put it back. */
        case SYS_PREADV:
        case SYS_PWRITEV: {
            struct process *process = process_current();
            int fd = (int)frame->rdi;
            int writing = syscall_number == SYS_PWRITEV;
            if (!process || fd < 0 || fd >= PROCESS_MAX_FDS || !process->files->fds[fd] ||
                process->files->fds[fd]->kind != FILE_KIND_VFS)
                frame->rax = (uint64_t)-(int64_t)EBADF;
            else {
                uint64_t saved = process->files->fds[fd]->offset;
                process->files->fds[fd]->offset = frame->r10;
                frame->rax = (uint64_t)sys_readv_writev(fd, frame->rsi, (int)frame->rdx, writing);
                process->files->fds[fd]->offset = saved;
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
        case SYS_ACCESS:
            frame->rax = (uint64_t)sys_faccess_at(AT_FDCWD, frame->rdi, (int)frame->rsi, 0);
            break;
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
        case SYS_FACCESSAT:
            frame->rax = (uint64_t)sys_faccess_at((int)frame->rdi, frame->rsi,
                                                  (int)frame->rdx, 0);
            break;
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
        case SYS_SCHED_GETAFFINITY:
            frame->rax = (uint64_t)sys_sched_getaffinity(frame->rdi,
                                                        (size_t)frame->rsi, frame->rdx);
            break;
        case SYS_SCHED_SETAFFINITY:
            frame->rax = (uint64_t)sys_sched_setaffinity(frame->rdi,
                                                        (size_t)frame->rsi, frame->rdx);
            if ((int64_t)frame->rax == 0) {
                uint64_t mask = 0;
                if (process_get_affinity(0, &mask) == 0 &&
                    !(mask & (1ULL << cpu_current()->index)))
                    process_yield_from_syscall(frame);
            }
            break;
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
            struct file *file = process && fd >= 0 && fd < PROCESS_MAX_FDS ? process->files->fds[fd] : NULL;
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
        case SYS_ACCEPT:
            accept_or_block(frame, SYS_ACCEPT, (int)frame->rdi, frame->rsi, frame->rdx, 0);
            break;
        case SYS_SENDTO: frame->rax = (uint64_t)sys_sendto((int)frame->rdi, frame->rsi, frame->rdx, (int)frame->r10, frame->r8, frame->r9); break;
        case SYS_RECVFROM: {
            int fd = (int)frame->rdi;
            int flags = (int)frame->r10;
            int64_t result = sys_recvfrom(fd, frame->rsi, frame->rdx, flags, frame->r8, frame->r9);
            struct process *process = process_current();
            struct file *file = process && fd >= 0 && fd < PROCESS_MAX_FDS ? process->files->fds[fd] : NULL;
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
        case SYS_SENDMMSG: frame->rax = (uint64_t)sys_sendmmsg((int)frame->rdi, frame->rsi, (unsigned)frame->rdx, (int)frame->r10); break;
        case SYS_RECVMSG: {
            int fd = (int)frame->rdi;
            int flags = (int)frame->rdx;
            int64_t result = sys_recvmsg(fd, frame->rsi, flags);
            struct process *process = process_current();
            struct file *file = process && fd >= 0 && fd < PROCESS_MAX_FDS ? process->files->fds[fd] : NULL;
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
        case SYS_WAITID: {
            /* idtype/id translated to wait4's pid convention; frame->r8 is the
             * struct rusage pointer, accepted and ignored exactly like wait4's. */
            int idtype = (int)frame->rdi;
            int64_t id = (int64_t)frame->rsi;
            int options = (int)frame->r10;
            int64_t pid_spec;
            if (idtype == 0) pid_spec = -1;                     /* P_ALL */
            else if (idtype == 1 && id > 0) pid_spec = id;      /* P_PID */
            else if (idtype == 2 && id > 0) pid_spec = -id;     /* P_PGID */
            else { frame->rax = (uint64_t)-(int64_t)EINVAL; break; }
            if ((options & ~(WNOHANG | WEXITED | WSTOPPED | WCONTINUED)) ||
                !(options & (WEXITED | WSTOPPED | WCONTINUED))) {
                frame->rax = (uint64_t)-(int64_t)EINVAL;
                break;
            }
            int64_t result = process_waitid_from_syscall(pid_spec, frame->rdx,
                                                         options);
            if (result == -EAGAIN) {
                if (!retry_io_wait(frame, SYS_WAITID, -1))
                    frame->rax = (uint64_t)-(int64_t)ECHILD;
            } else {
                clear_io_wait(process_current());
                frame->rax = (uint64_t)result;
            }
            break;
        }
        case SYS_KILL: frame->rax = (uint64_t)process_send_signal_checked((int64_t)frame->rdi, (int)frame->rsi); break;
        /* Single-threaded here: a tid is a pid, so tkill(tid,sig) is kill(pid,sig).
         * musl's raise()/abort() route through tkill. */
        case SYS_TKILL: frame->rax = (uint64_t)process_send_signal_checked((int64_t)frame->rdi, (int)frame->rsi); break;
        case SYS_TGKILL: frame->rax = (uint64_t)process_send_signal_checked((int64_t)frame->rsi, (int)frame->rdx); break;
        case SYS_UNAME: frame->rax = (uint64_t)sys_uname(frame->rdi); break;
        case SYS_SETHOSTNAME:
            frame->rax = (uint64_t)set_machine_name(0, frame->rdi, frame->rsi);
            break;
        case SYS_SETDOMAINNAME:
            frame->rax = (uint64_t)set_machine_name(1, frame->rdi, frame->rsi);
            break;
        case SYS_FCNTL: {
            struct process *process = process_current();
            int fd = (int)frame->rdi;
            int command = (int)frame->rsi;
            if (!process || fd < 0 || fd >= PROCESS_MAX_FDS || !process->files->fds[fd]) {
                frame->rax = (uint64_t)-(int64_t)EBADF;
            } else if (command == F_DUPFD || command == F_DUPFD_CLOEXEC) {
                frame->rax = (uint64_t)sys_dup(fd, (int)frame->rdx,
                    command == F_DUPFD_CLOEXEC);
            } else if (command == F_GETFD) {
                frame->rax = (process->files->fd_flags[fd] & PROCESS_FD_CLOEXEC) ? FD_CLOEXEC : 0;
            } else if (command == F_SETFD) {
                if (frame->rdx & ~(uint64_t)FD_CLOEXEC)
                    frame->rax = (uint64_t)-(int64_t)EINVAL;
                else {
                    process->files->fd_flags[fd] = (frame->rdx & FD_CLOEXEC) ? PROCESS_FD_CLOEXEC : 0;
                    frame->rax = 0;
                }
            } else if (command == F_GETLK || command == F_SETLK || command == F_SETLKW) {
                /* F_SETLKW would wait; with whole-file granularity and a
                   single contending process there is nothing to wait for, so
                   it is answered like F_SETLK. */
                frame->rax = (uint64_t)sys_fcntl_lock(fd, command, frame->rdx);
            } else if (command == F_ADD_SEALS || command == F_GET_SEALS) {
                /* Only a memfd has seals; every other kind answers EINVAL,
                   which is how a caller asks whether this one can be sealed. */
                struct file *file = process->files->fds[fd];
                if (file->kind != FILE_KIND_MEMFD) {
                    frame->rax = (uint64_t)-(int64_t)EINVAL;
                } else if (command == F_GET_SEALS) {
                    frame->rax = memfd_seals(file->memfd);
                } else {
                    frame->rax = (uint64_t)(int64_t)
                        memfd_add_seals(file->memfd, (uint32_t)frame->rdx);
                }
            } else if (command == F_GETFL) {
                frame->rax = process->files->fds[fd]->flags;
            } else if (command == F_SETFL) {
                process->files->fds[fd]->flags =
                    (process->files->fds[fd]->flags & ~(uint32_t)O_NONBLOCK) |
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
        case SYS_REBOOT:
            frame->rax = (uint64_t)sys_reboot((uint32_t)frame->rdi,
                                              (uint32_t)frame->rsi,
                                              (uint32_t)frame->rdx);
            break;
        case SYS_FTRUNCATE: frame->rax = (uint64_t)sys_ftruncate((int)frame->rdi, frame->rsi); break;
        case SYS_FLOCK: {
            int operation = (int)frame->rsi;
            int64_t result = sys_flock((int)frame->rdi, operation);
            /* Without LOCK_NB a contended lock waits instead of failing. The
               retry rewinds the syscall and yields, so the process re-attempts
               it after other work runs -- there is no per-lock wait queue, and
               a lock this coarse is not worth one. */
            if (result == -EAGAIN && !(operation & FILE_LOCK_NB)) {
                if (!retry_io_wait(frame, SYS_FLOCK, -1))
                    frame->rax = (uint64_t)result;
            } else {
                clear_io_wait(process_current());
                frame->rax = (uint64_t)result;
            }
            break;
        }
        case SYS_MEMFD_CREATE:
            frame->rax = (uint64_t)sys_memfd_create(frame->rdi, (uint32_t)frame->rsi);
            break;
        case SYS_FALLOCATE:
            frame->rax = (uint64_t)sys_fallocate((int)frame->rdi, (int)frame->rsi,
                                                 frame->rdx, frame->r10);
            break;
        /* The legacy signalfd takes no flags; signalfd4 is what libc calls. */
        case SYS_SIGNALFD:
            frame->rax = (uint64_t)sys_signalfd((int)frame->rdi, frame->rsi,
                                                frame->rdx, 0);
            break;
        case SYS_SIGNALFD4:
            frame->rax = (uint64_t)sys_signalfd((int)frame->rdi, frame->rsi,
                                                frame->rdx, (int)frame->r10);
            break;
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
        case SYS_CHOWN:
            frame->rax = (uint64_t)sys_chown_at(AT_FDCWD, frame->rdi, (uint32_t)frame->rsi,
                                                (uint32_t)frame->rdx, 0);
            break;
        case SYS_LCHOWN:
            frame->rax = (uint64_t)sys_chown_at(AT_FDCWD, frame->rdi, (uint32_t)frame->rsi,
                                                (uint32_t)frame->rdx, AT_SYMLINK_NOFOLLOW);
            break;
        case SYS_FCHOWN:
            frame->rax = (uint64_t)sys_fchown((int)frame->rdi, (uint32_t)frame->rsi,
                                              (uint32_t)frame->rdx);
            break;
        case SYS_FCHOWNAT:
            frame->rax = (uint64_t)sys_chown_at((int)frame->rdi, frame->rsi, (uint32_t)frame->rdx,
                                                (uint32_t)frame->r10, (int)frame->r8);
            break;
        case SYS_UMASK: frame->rax = process_set_umask((uint32_t)frame->rdi); break;
        case SYS_GETTIMEOFDAY: frame->rax = (uint64_t)sys_gettimeofday(frame->rdi); break;
        case SYS_GETRLIMIT: frame->rax = (uint64_t)sys_prlimit(frame->rdi, frame->rsi); break;
        /*
         * Nice selects a weight in the ordinary scheduler band. getpriority
         * reports 20 - nice so that a negative nice is not
         * mistaken for an error, and pam_limits treats a failure here as a
         * reason to abort the whole session.
         *
         * PRIO_PROCESS is the only `which` that means anything to this: a
         * process group or a user is not something the scheduler knows.
         */
        case SYS_GETPRIORITY: {
            int nice = 0;
            frame->rax = process_get_nice(frame->rdi == 0 ? frame->rsi : 0, &nice) == 0
                             ? (uint64_t)(20 - nice) : 20;
            break;
        }
        case SYS_SETPRIORITY: {
            int result = process_set_nice(frame->rdi == 0 ? frame->rsi : 0,
                                          (int)(int32_t)frame->rdx);
            frame->rax = result == 0 ? 0 : (uint64_t)(int64_t)result;
            break;
        }
        case SYS_GETRUSAGE: frame->rax = (uint64_t)sys_getrusage(frame->rsi); break;
        /*
         * Scheduling policy, which a thread may now actually have. What it
         * buys is being taken off the runnable list first; see next_runnable()
         * in process.c, and docs/sound.md for the program that asked.
         *
         * A thread id, not a process id: pthread_setschedparam passes one, and
         * the thread that wants this is one of several inside a process.
         */
        case SYS_SCHED_GETSCHEDULER: {
            int policy = 0;
            int result = process_get_scheduler(frame->rdi, &policy, NULL);
            frame->rax = result == 0 ? (uint64_t)policy : (uint64_t)(int64_t)result;
            break;
        }
        case SYS_SCHED_SETSCHEDULER: {
            uint32_t priority = 0;
            if (frame->rdx &&
                copy_from_user(&priority, frame->rdx, sizeof(priority)) != 0) {
                frame->rax = (uint64_t)-(int64_t)EFAULT;
                break;
            }
            int result = process_set_scheduler(frame->rdi, (int)frame->rsi,
                                               (int)priority);
            frame->rax = result == 0 ? 0 : (uint64_t)(int64_t)result;
            break;
        }
        case SYS_SCHED_GETPARAM:
        case SYS_SCHED_SETPARAM: {
            uint32_t priority = 0;
            if (syscall_number == SYS_SCHED_SETPARAM) {
                if (copy_from_user(&priority, frame->rsi, sizeof(priority)) != 0) {
                    frame->rax = (uint64_t)-(int64_t)EFAULT;
                    break;
                }
                /* The policy stays what it was; only the number inside it
                   moves, which is what sched_setparam means. */
                int policy = PROCESS_SCHED_OTHER;
                int result = process_get_scheduler(frame->rdi, &policy, NULL);
                if (result == 0)
                    result = process_set_scheduler(frame->rdi, policy, (int)priority);
                frame->rax = result == 0 ? 0 : (uint64_t)(int64_t)result;
                break;
            }
            int stored = 0;
            int result = process_get_scheduler(frame->rdi, NULL, &stored);
            if (result != 0) {
                frame->rax = (uint64_t)(int64_t)result;
                break;
            }
            priority = (uint32_t)stored;
            frame->rax = copy_to_user(frame->rsi, &priority, sizeof(priority)) == 0
                             ? 0 : (uint64_t)-(int64_t)EFAULT;
            break;
        }
        /* Both zero, which is what Linux answers for SCHED_OTHER. */
        /* The range a policy's priority may take. Zero for the ordinary band
           and 1..99 for the real-time ones, as everywhere else. */
        case SYS_SCHED_GET_PRIORITY_MAX:
            frame->rax = (frame->rdi == PROCESS_SCHED_FIFO ||
                          frame->rdi == PROCESS_SCHED_RR)
                             ? PROCESS_RT_PRIORITY_MAX : 0;
            break;
        case SYS_SCHED_GET_PRIORITY_MIN:
            frame->rax = (frame->rdi == PROCESS_SCHED_FIFO ||
                          frame->rdi == PROCESS_SCHED_RR) ? 1 : 0;
            break;
        case SYS_SCHED_RR_GET_INTERVAL: {
            struct { int64_t seconds; int64_t nanoseconds; } slice = {0, 0};
            frame->rax = copy_to_user(frame->rsi, &slice, sizeof(slice)) == 0
                ? 0 : (uint64_t)-(int64_t)EFAULT;
            break;
        }
        case SYS_GETCPU: {
            uint32_t cpu = cpu_current() ? cpu_current()->index : 0;
            uint32_t node = 0;
            if (frame->rdi && copy_to_user(frame->rdi, &cpu, sizeof(cpu)) != 0)
                frame->rax = (uint64_t)-(int64_t)EFAULT;
            else if (frame->rsi && copy_to_user(frame->rsi, &node, sizeof(node)) != 0)
                frame->rax = (uint64_t)-(int64_t)EFAULT;
            else frame->rax = 0;
            break;
        }
        /* Every processor takes the kernel lock to run a syscall, so a syscall
           returning is already the barrier this asks for. */
        case SYS_MEMBARRIER: frame->rax = 0; break;
        case SYS_SYSINFO: frame->rax = (uint64_t)sys_sysinfo(frame->rdi); break;
        /* time(2). glibc calls it rather than clock_gettime when it only wants
           whole seconds, which procps does once per row it prints. */
        case SYS_TIME: {
            int64_t seconds = (int64_t)time_epoch_seconds();
            if (frame->rdi && copy_to_user(frame->rdi, &seconds, sizeof(seconds)) != 0)
                frame->rax = (uint64_t)-(int64_t)EFAULT;
            else frame->rax = (uint64_t)seconds;
            break;
        }
        case SYS_TIMES: frame->rax = (uint64_t)sys_times(frame->rdi); break;
        case SYS_GETUID: frame->rax = cred_current() ? cred_current()->uid : 0; break;
        case SYS_GETGID: frame->rax = cred_current() ? cred_current()->gid : 0; break;
        case SYS_GETEUID: frame->rax = cred_current() ? cred_current()->euid : 0; break;
        case SYS_GETEGID: frame->rax = cred_current() ? cred_current()->egid : 0; break;
        case SYS_SETUID: frame->rax = (uint64_t)cred_set_uid((uint32_t)frame->rdi); break;
        case SYS_SETGID: frame->rax = (uint64_t)cred_set_gid((uint32_t)frame->rdi); break;
        case SYS_SETREUID:
            frame->rax = (uint64_t)cred_set_reuid((uint32_t)frame->rdi, (uint32_t)frame->rsi);
            break;
        case SYS_SETREGID:
            frame->rax = (uint64_t)cred_set_regid((uint32_t)frame->rdi, (uint32_t)frame->rsi);
            break;
        case SYS_SETRESUID:
            frame->rax = (uint64_t)cred_set_resuid((uint32_t)frame->rdi, (uint32_t)frame->rsi,
                                                   (uint32_t)frame->rdx);
            break;
        case SYS_SETRESGID:
            frame->rax = (uint64_t)cred_set_resgid((uint32_t)frame->rdi, (uint32_t)frame->rsi,
                                                   (uint32_t)frame->rdx);
            break;
        case SYS_GETRESUID: frame->rax = (uint64_t)sys_getresuid(frame->rdi, frame->rsi, frame->rdx, 0); break;
        case SYS_GETRESGID: frame->rax = (uint64_t)sys_getresuid(frame->rdi, frame->rsi, frame->rdx, 1); break;
        case SYS_SETFSUID: frame->rax = (uint64_t)cred_set_fsuid((uint32_t)frame->rdi); break;
        case SYS_SETFSGID: frame->rax = (uint64_t)cred_set_fsgid((uint32_t)frame->rdi); break;
        case SYS_GETGROUPS:
            frame->rax = (uint64_t)sys_getgroups((int64_t)frame->rdi, frame->rsi);
            break;
        case SYS_SETGROUPS:
            frame->rax = (uint64_t)sys_setgroups((int64_t)frame->rdi, frame->rsi);
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
        case SYS_CAPGET: frame->rax = (uint64_t)sys_capget(frame->rdi, frame->rsi); break;
        case SYS_CAPSET: frame->rax = (uint64_t)sys_capset(frame->rdi, frame->rsi); break;
        case SYS_SIGALTSTACK: frame->rax = (uint64_t)sys_sigaltstack(frame, frame->rdi, frame->rsi); break;
        case SYS_PRCTL: frame->rax = (uint64_t)sys_prctl((int)frame->rdi, frame->rsi, frame->rdx, frame->r10, frame->r8); break;
        case SYS_ARCH_PRCTL: frame->rax = (uint64_t)sys_arch_prctl((int)frame->rdi, frame->rsi); break;
        case SYS_FUTEX: {
            int operation = (int)frame->rsi;
            int command = operation & FUTEX_CMD_MASK;
            int bitset_form = command == FUTEX_WAIT_BITSET || command == FUTEX_WAKE_BITSET;
            /* val3 carries the bitset; the plain commands mean all of it. */
            uint32_t bitset = bitset_form ? (uint32_t)frame->r9 : FUTEX_BITSET_MATCH_ANY;
            if (!bitset) {
                frame->rax = (uint64_t)-(int64_t)EINVAL;
                break;
            }
            if (command == FUTEX_WAKE || command == FUTEX_WAKE_BITSET) {
                frame->rax = (uint64_t)process_futex_wake(frame->rdi, (int)frame->rdx, bitset);
            } else if (command == FUTEX_WAIT || command == FUTEX_WAIT_BITSET) {
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
                    /* A bitset wait names the moment to give up rather than
                       how long to wait, against whichever clock the operation
                       chose. Turn it into the duration the wait wants. */
                    if (command == FUTEX_WAIT_BITSET) {
                        uint64_t now = (operation & FUTEX_CLOCK_REALTIME)
                            ? time_realtime_ns() : time_uptime_ns();
                        timeout_ns = (uint64_t)timeout_ns > now
                            ? (int64_t)((uint64_t)timeout_ns - now) : 0;
                    }
                }
                struct process *futex_caller = process_current();
                int64_t result = process_futex_wait(frame, frame->rdi, (uint32_t)frame->rdx,
                                                    timeout_ns, bitset);
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
        case SYS_MKNOD:
            frame->rax = (uint64_t)sys_mknodat(AT_FDCWD, frame->rdi,
                                               (uint32_t)frame->rsi, frame->rdx);
            break;
        case SYS_MKNODAT:
            frame->rax = (uint64_t)sys_mknodat((int)frame->rdi, frame->rsi,
                                               (uint32_t)frame->rdx, frame->r10);
            break;
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
        case SYS_MOUNT: frame->rax = (uint64_t)sys_mount(frame->rdi, frame->rsi, frame->rdx, frame->r10, frame->r8); break;
        case SYS_UMOUNT2: frame->rax = (uint64_t)sys_umount2(frame->rdi, (int)frame->rsi); break;
        case SYS_LINK: frame->rax = (uint64_t)sys_link_at(AT_FDCWD, frame->rdi, AT_FDCWD, frame->rsi, 0); break;
        case SYS_LINKAT: frame->rax = (uint64_t)sys_link_at((int)frame->rdi, frame->rsi, (int)frame->rdx, frame->r10, (int)frame->r8); break;
        case SYS_READLINKAT: frame->rax = (uint64_t)sys_readlink_at((int)frame->rdi, frame->rsi, frame->rdx, (size_t)frame->r10); break;
        case SYS_FCHMODAT: frame->rax = (uint64_t)sys_chmod_at((int)frame->rdi, frame->rsi, (uint32_t)frame->rdx, 0); break;
        case SYS_UTIMENSAT:
            frame->rax = (uint64_t)sys_utimens_at((int)frame->rdi, frame->rsi,
                                                  frame->rdx, (int)frame->r10);
            break;
        case SYS_SET_ROBUST_LIST: frame->rax = (uint64_t)sys_set_robust_list(frame->rdi, (size_t)frame->rsi); break;
        case SYS_GET_ROBUST_LIST: frame->rax = (uint64_t)sys_get_robust_list((int)frame->rdi, frame->rsi, frame->rdx); break;
        case SYS_ACCEPT4:
            accept_or_block(frame, SYS_ACCEPT4, (int)frame->rdi, frame->rsi, frame->rdx, (int)frame->r10);
            break;
        case SYS_PIPE2: frame->rax = (uint64_t)sys_pipe(frame->rdi, (int)frame->rsi); break;
        case SYS_PRLIMIT64: frame->rax = (uint64_t)sys_prlimit(frame->rsi, frame->r10); break;
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
        case SYS_FACCESSAT2:
            frame->rax = (uint64_t)sys_faccess_at((int)frame->rdi, frame->rsi,
                                                  (int)frame->rdx, (int)frame->r10);
            break;
        case SYS_CLOSE_RANGE: {
            struct process *process = process_current();
            uint64_t first = frame->rdi, last = frame->rsi, flags = frame->rdx;
            if (!process || first >= PROCESS_MAX_FDS) frame->rax = 0;
            else {
                if (last >= PROCESS_MAX_FDS) last = PROCESS_MAX_FDS - 1;
                if (flags & CLOSE_RANGE_CLOEXEC) {
                    /* Mark the range close-on-exec rather than closing it now.
                       glib/VTE's between-fork-and-exec spawn code calls
                       close_range(3, ~0, CLOSE_RANGE_CLOEXEC) expecting its
                       error-report pipe to stay open until the child execs;
                       closing it here made every terminal spawn look like an
                       instant child exit. */
                    for (uint64_t fd = first; fd <= last; fd++)
                        if (process->files->fds[fd])
                            process_set_fd_flags(process, (int)fd, PROCESS_FD_CLOEXEC);
                } else {
                    for (uint64_t fd = first; fd <= last; fd++)
                        if (process->files->fds[fd]) process_close_fd(process, (int)fd);
                }
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

/*
 * The frame arrived on the kernel stack of the process that made the call, and
 * that process may not be the one this return path is heading back to: a
 * blocking syscall gives its processor away and the return frame becomes
 * somebody else's. The stack it is sitting on then belongs to a process that
 * another processor is free to schedule -- and the first thing that process
 * does on entering the kernel is write over exactly these bytes.
 *
 * So the frame is moved onto the kernel stack of whichever process is running
 * here now -- a stack nobody else can enter while this processor holds the
 * process -- and syscall_entry.S reloads rsp from the same per-CPU field, so
 * the two always agree. When nothing was switched, source and destination are
 * the same address and no copy happens.
 *
 * This function does not drop the lock. It cannot: its own return address is
 * on the stack it is trying to get off, and a `ret` executed after the lock is
 * gone reads a stack that may already have been reaped and its pages given
 * back. syscall_entry.S drops it, once rsp is somewhere safe.
 */
_Static_assert(sizeof(struct syscall_frame) == 144, "syscall_entry.S assumes 144");

/*
 * Which files a read or a write may move bytes through without excluding the
 * rest of the kernel.
 *
 * The test is not "is this fast" but "is everything it touches either private
 * to this process or behind a lock of its own". A pipe qualifies because it
 * has a lock now and its whole state is inside it. A character device whose
 * handler keeps no state -- /dev/zero, /dev/null and their kind -- qualifies
 * because there is nothing to protect. Everything else, including regular
 * files (whose writes reach the disk and the persistence hooks), sockets, ptys
 * and terminals, does not, and takes the kernel lock exclusively as before.
 */
static int file_may_share(const struct file *file) {
    if (!file) return 0;
    if (file->kind == FILE_KIND_PIPE_READ || file->kind == FILE_KIND_PIPE_WRITE)
        return 1;
    if (file->kind != FILE_KIND_VFS || !file->node) return 0;
    /* A stateless device: no data of its own, so its handler cannot race. */
    if ((file->node->flags & 0xFFU) != VFS_CHARDEVICE) return 0;
    return file->node->data == NULL && file->node->length == 0;
}

/*
 * A read or a write, attempted without excluding anyone. Returns zero when it
 * could not be finished this way -- an unsuitable file, or one that would have
 * blocked, since blocking means the scheduler and the scheduler is exclusive.
 * The caller then starts again with the kernel lock held, which is what every
 * other syscall does from the outset.
 */
static int syscall_try_shared(struct syscall_frame *frame) {
    uint64_t number = frame->rax;
    int fd = (int)frame->rdi;
    struct process *process = process_current();
    if (!process || !process->files || fd < 0 || fd >= PROCESS_MAX_FDS) return 0;
    struct file *file = process->files->fds[fd];
    if (!file_may_share(file)) return 0;

    int64_t result = number == SYS_READ
        ? sys_read(fd, frame->rsi, (size_t)frame->rdx)
        : sys_write(fd, frame->rsi, (size_t)frame->rdx);

    /* Would have blocked: leave it to the exclusive path, which can sleep. */
    if (result == -EAGAIN && !(file->flags & O_NONBLOCK)) return 0;

    frame->rax = (uint64_t)result;
    return 1;
}

/* Cheap enough to ask before any lock is held, because it reads nothing but
   the number the process passed in a register. */
static int syscall_number_may_share(uint64_t number) {
    return number == SYS_READ || number == SYS_WRITE;
}

#define VERBOSE_SYSCALL_LIMIT 24U

void syscall_dispatch(struct syscall_frame *frame) {
    klock_note(KLOCK_NOTE_SYSCALL | (uint32_t)frame->rax);
    /* The first syscall is the answer to one question and it is the question
       that matters here: whether userland ran at all. */
    if (boot_verbose()) {
        static unsigned traced;
        if (traced < VERBOSE_SYSCALL_LIMIT) {
            traced++;
            kprintf("syscall: %u from pid %d\n", (unsigned)frame->rax,
                    (int)process_current_pid());
        }
    }

    if (syscall_number_may_share(frame->rax)) {
        kernel_lock_shared();
        if (syscall_try_shared(frame)) {
            /* The entry stub releases whichever mode is held. */
            uint64_t top = cpu_current()->kernel_rsp;
            if (top) {
                struct syscall_frame *resumed =
                    (struct syscall_frame *)(top - sizeof(*frame));
                if (resumed != frame) *resumed = *frame;
            }
            return;
        }
        kernel_unlock_shared();
    }

    kernel_lock();
    syscall_dispatch_locked(frame);
    uint64_t stack_top = cpu_current()->kernel_rsp;
    if (stack_top) {
        struct syscall_frame *resumed =
            (struct syscall_frame *)(stack_top - sizeof(*frame));
        if (resumed != frame) *resumed = *frame;
    }
}
