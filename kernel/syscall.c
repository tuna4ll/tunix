#include <stddef.h>
#include <stdint.h>
#include "include/abi_gaps.h"
#include "include/cred.h"
#include "include/file.h"
#include "include/eventfd.h"
#include "include/eventfs.h"
#include "include/timerfd.h"
#include "include/epoll.h"
#include "include/inotify.h"
#include "include/memfd.h"
#include "include/module.h"
#include "include/signalfd.h"
#include "include/sysvshm.h"
#include "include/framebuffer.h"
#include "include/input.h"
#include "include/heap.h"
#include "include/kentry.h"
#include "include/klog.h"
#include "include/percpu.h"
#include "include/defer.h"
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

#if defined(__x86_64__)
_Static_assert(sizeof(struct syscall_frame) == 144, "syscall frame/assembly ABI mismatch");
_Static_assert(offsetof(struct syscall_frame, rax) == 96, "syscall frame rax offset mismatch");
_Static_assert(offsetof(struct syscall_frame, rcx) == 104, "syscall frame rcx offset mismatch");
_Static_assert(offsetof(struct syscall_frame, r11) == 112, "syscall frame r11 offset mismatch");
_Static_assert(offsetof(struct syscall_frame, user_rip) == 120, "syscall frame rip offset mismatch");
_Static_assert(offsetof(struct syscall_frame, user_rsp) == 136, "syscall frame rsp offset mismatch");
#endif

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
#define SYS_RECVMMSG 299
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
#define SYS_INIT_MODULE 175
#define SYS_DELETE_MODULE 176
#define SYS_FINIT_MODULE 313
#define SYS_SYNCFS 306
#define SYS_FTRUNCATE 77
#define SYS_GETCWD 79
#define SYS_CHDIR 80
#define SYS_FCHDIR 81
#define SYS_CHROOT 161
#define SYS_SETXATTR 188
#define SYS_LSETXATTR 189
#define SYS_FSETXATTR 190
#define SYS_GETXATTR 191
#define SYS_LGETXATTR 192
#define SYS_FGETXATTR 193
#define SYS_LISTXATTR 194
#define SYS_LLISTXATTR 195
#define SYS_FLISTXATTR 196
#define SYS_REMOVEXATTR 197
#define SYS_LREMOVEXATTR 198
#define SYS_FREMOVEXATTR 199
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
#define SYS_SETRLIMIT 160
#define SYS_GETRUSAGE 98
#define SYS_SYSLOG 103
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
#define MSG_WAITFORONE 0x10000
#define SOCKET_MESSAGE_STAGE 4096
#define SOCKET_MESSAGE_MAX PIPE_CAPACITY
#define UIO_MAXIOV 1024
#define SO_SNDBUF 7
#define SO_RCVBUF 8
#define MSG_CTRUNC 0x08
#define IPPROTO_IP_LEVEL 0
#define IP_TTL_OPTION 2
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
#define MFD_NOEXEC_SEAL 0x0008U
#define MFD_EXEC 0x0010U
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

#define MAP_FIXED_NOREPLACE 0x100000
#define MS_ASYNC 1
#define MS_INVALIDATE 2
#define MS_SYNC 4

#define F_DUPFD 0
#define F_GETFD 1
#define F_SETFD 2
#define F_GETFL 3
#define F_SETFL 4
#define F_ADD_SEALS 1033
#define F_SETPIPE_SZ 1031
#define F_GETPIPE_SZ 1032
#define F_GET_SEALS 1034
#define FIONBIO 0x5421UL
#define FIONREAD 0x541BUL
#define F_GETLK 5
#define F_SETLK 6
#define F_SETLKW 7
#define F_RDLCK 0
#define F_WRLCK 1
#define F_UNLCK 2

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
#define CLONE_PIDFD           0x00001000ULL
#define CLONE_VFORK           0x00004000ULL
#define CLONE_PARENT          0x00008000ULL
#define CLONE_THREAD          0x00010000ULL
#define CLONE_SYSVSEM         0x00040000ULL
#define CLONE_SETTLS          0x00080000ULL
#define CLONE_PARENT_SETTID   0x00100000ULL
#define CLONE_CHILD_CLEARTID  0x00200000ULL
#define CLONE_DETACHED        0x00400000ULL
#define CLONE_CHILD_SETTID    0x01000000ULL
#define CLONE_CLEAR_SIGHAND   (1ULL << 32)
#define CLONE_INTO_CGROUP     (1ULL << 33)
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

#define ARCH_SET_GS 0x1001
#define ARCH_SET_FS 0x1002
#define ARCH_GET_FS 0x1003
#define ARCH_GET_GS 0x1004

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
#define ENODATA 61
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

#define FUTEX_WAIT_BITSET 9
#define FUTEX_WAKE_BITSET 10
#define FUTEX_PRIVATE_FLAG 128

#define FUTEX_CLOCK_REALTIME 256
#define FUTEX_CMD_MASK 0x7F

#define MAX_ARG_STRLEN (128U * 1024U)

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

struct linux_rlimit {
    uint64_t rlim_cur;
    uint64_t rlim_max;
};

#if defined(__x86_64__)
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
#elif defined(__aarch64__)
struct linux_stat {
    uint64_t st_dev;
    uint64_t st_ino;
    uint32_t st_mode;
    uint32_t st_nlink;
    uint32_t st_uid;
    uint32_t st_gid;
    uint64_t st_rdev;
    uint64_t __pad1;
    int64_t st_size;
    int32_t st_blksize;
    int32_t __pad2;
    int64_t st_blocks;
    struct linux_timespec st_atim;
    struct linux_timespec st_mtim;
    struct linux_timespec st_ctim;
    uint32_t __unused[2];
};

_Static_assert(sizeof(struct linux_stat) == 128, "arm64 struct stat is 128 bytes");
#endif

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

struct exec_vector {
    const char **items;
    size_t count;
    size_t capacity;
};

struct exec_arguments {
    size_t budget;
    size_t used;
    struct exec_vector argv;
    struct exec_vector envp;
};

static int nx_enabled;

static inline uint64_t align_up(uint64_t value, uint64_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

#if defined(__x86_64__)

extern void syscall_entry(void);

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

#else

void syscall_init(void) {
    nx_enabled = 1;
}

#endif

#define WRITE_STAGE_MAX (128U * 1024U)

static int write_stages_large(const struct file *file) {
    if (!file) return 0;
    if (file->kind == FILE_KIND_SOCKET || file->kind == FILE_KIND_PIPE_WRITE)
        return 1;
    return file->kind == FILE_KIND_VFS && file->node &&
           (file->node->flags & 0xFFU) == VFS_FILE;
}


static struct file_pins spare_pins[SMP_MAX_CPUS];
static struct file_pins orphan_pins[SMP_MAX_CPUS];

static struct file_pins *pins_here(void) {
    struct process *process = process_current();
    return process ? &process->pins : &spare_pins[cpu_current()->index];
}

static int pin_file(struct file *file) {
    struct file_pins *pins = pins_here();
    if (pins->count == pins->capacity) {
        unsigned capacity = pins->capacity ? pins->capacity * 2U : 16U;
        struct file **files = (struct file **)kmalloc(capacity * sizeof(*files));
        if (!files) return -1;
        if (pins->count) memcpy(files, pins->files, pins->count * sizeof(*files));
        kfree(pins->files);
        pins->files = files;
        pins->capacity = capacity;
    }
    pins->files[pins->count++] = file;
    return 0;
}

static struct file *fd_file(int fd) {
    struct file *file = process_file_get(process_current(), fd);
    if (!file) return NULL;
    if (pin_file(file) != 0) {
        file_unref(file);
        return NULL;
    }
    return file;
}

void syscall_unref_later(struct file *file) {
    if (!file) return;
    if (pin_file(file) != 0) file_unref(file);
}

static void release_set(struct file_pins *pins) {
    while (pins->count) file_unref(pins->files[--pins->count]);
}

void syscall_release_pins_of(struct process *process) {
    if (process) release_set(&process->pins);
}

void syscall_release_pins(void) {
    release_set(pins_here());
}

void syscall_orphan_pins(struct process *process) {
    struct file_pins *orphans = &orphan_pins[cpu_current()->index];
    if (!process || !process->pins.count || orphans->count) return;
    struct file_pins swap = *orphans;
    *orphans = process->pins;
    process->pins = swap;
}

void syscall_release_orphans(void) {
    unsigned index = cpu_current()->index;
    release_set(&orphan_pins[index]);
    release_set(&spare_pins[index]);
}

static int64_t sys_write(int fd, uint64_t user_buffer, size_t length) {
    struct process *process = process_current();
    struct file *file = fd_file(fd);
    if (!file) return -EBADF;
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
    if (completed && eventfs_interested(EVENTFS_FILES, process->cred.euid, 0) &&
        file->kind == FILE_KIND_VFS && file->node &&
        (file->node->flags & 0xFFU) == VFS_FILE) {
        VFS_PATH_SCOPED path = vfs_path_buffer();
        if (path && vfs_node_path(file->node, path, VFS_PATH_MAX) == 0)
            eventfs_emit_file_write(process->cred.euid, process->tgid, path);
    }
    return (int64_t)completed;
}

static int64_t sys_read(int fd, uint64_t user_buffer, size_t length) {
    struct process *process = process_current();
    if (!process || fd < 0 || fd >= PROCESS_FD_CAPACITY(process) || !fd_file(fd)) return -EBADF;
    uint8_t buffer[4096];
    size_t completed = 0;
    while (completed < length) {
        size_t chunk = length - completed;
        if (chunk > sizeof(buffer)) chunk = sizeof(buffer);
        int64_t amount = file_read(fd_file(fd), chunk, buffer);
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
    process->io_watch_armed = 0;
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

    if (process_signal_interrupts_wait()) {
        clear_io_wait(waiting);
        SYSCALL_RET(frame) = (uint64_t)-(int64_t)EINTR;
        return 1;
    }

    SYSCALL_RESTART(frame, syscall_number);
    waiting->syscall_rewound = 1;

    if (process_sleep_on(frame, process_io_wait_channel()) != 0)
        process_yield_from_syscall(frame);
    return 1;
}

static void io_watch_begin(struct process *process) {
    if (!process) return;
    process->io_watch_count = 0;
    process->io_watch_armed = 1;
}

static void io_watch_add(struct process *process, int fd, uint32_t events) {
    if (!process || !process->io_watch_armed) return;
    if (process->io_watch_count >= process->io_watch_capacity) {
        unsigned capacity = process->io_watch_capacity ? process->io_watch_capacity * 2 : 16;
        int *fds = (int *)kmalloc(capacity * sizeof(*fds));
        uint32_t *events_list = (uint32_t *)kmalloc(capacity * sizeof(*events_list));
        if (!fds || !events_list) {
            kfree(fds);
            kfree(events_list);
            process->io_watch_armed = 0;
            return;
        }
        if (process->io_watch_count) {
            memcpy(fds, process->io_watch_fd, process->io_watch_count * sizeof(*fds));
            memcpy(events_list, process->io_watch_events,
                   process->io_watch_count * sizeof(*events_list));
        }
        kfree(process->io_watch_fd);
        kfree(process->io_watch_events);
        process->io_watch_fd = fds;
        process->io_watch_events = events_list;
        process->io_watch_capacity = capacity;
    }
    process->io_watch_fd[process->io_watch_count] = fd;
    process->io_watch_events[process->io_watch_count] = events;
    process->io_watch_count++;
}

static void release_pollfds(struct linux_pollfd **fds) {
    kfree(*fds);
}

static int64_t sys_poll_once(uint64_t user_fds, uint64_t count, int commit_empty) {
    struct process *process = process_current();
    if (!process || count > process->rlimits[PROCESS_RLIMIT_NOFILE].soft) return -EINVAL;
    size_t bytes = (size_t)count * sizeof(struct linux_pollfd);
    __attribute__((cleanup(release_pollfds))) struct linux_pollfd *fds =
        (struct linux_pollfd *)kmalloc(bytes ? bytes : 1);
    if (!fds) return -ENOMEM;
    if (bytes && copy_from_user(fds, user_fds, bytes) != 0) return -EFAULT;

    int ready = 0;
    for (uint64_t i = 0; i < count; i++) {
        fds[i].revents = 0;
        int fd = fds[i].fd;
        if (fd < 0) continue;
        if (fd >= PROCESS_FD_CAPACITY(process) || !fd_file(fd)) {
            fds[i].revents = POLLNVAL;
            ready++;
            continue;
        }
        struct file *file = fd_file(fd);
        fds[i].revents = (int16_t)file_poll_events(file, (uint32_t)(uint16_t)fds[i].events);
        if (fds[i].revents) ready++;
    }
    if (!ready && process) {
        io_watch_begin(process);
        for (uint64_t i = 0; i < count; i++)
            if (fds[i].fd >= 0) io_watch_add(process, fds[i].fd, (uint32_t)(uint16_t)fds[i].events);
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

struct linux_sigset_argument {
    uint64_t set;
    uint64_t size;
};

static void apply_wait_signal_mask(uint64_t user_argument) {
    if (!user_argument) return;
    struct linux_sigset_argument argument;
    if (copy_from_user(&argument, user_argument, sizeof(argument)) != 0) return;
    if (!argument.set || argument.size != sizeof(uint64_t)) return;
    uint64_t mask;
    if (copy_from_user(&mask, argument.set, sizeof(mask)) != 0) return;
    process_swap_signal_mask(mask);
}

static void apply_wait_signal_set(uint64_t user_set, uint64_t size) {
    if (!user_set || size != sizeof(uint64_t)) return;
    uint64_t mask;
    if (copy_from_user(&mask, user_set, sizeof(mask)) != 0) return;
    process_swap_signal_mask(mask);
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

static int bits_test(const uint64_t *bits, int fd) {
    return (bits[(unsigned)fd / 64U] >> ((unsigned)fd % 64U)) & 1U;
}

static void bits_put(uint64_t *bits, int fd) {
    bits[(unsigned)fd / 64U] |= 1ULL << ((unsigned)fd % 64U);
}

static void release_bits(uint64_t **bits) {
    kfree(*bits);
}

static int64_t sys_select_once(int nfds, uint64_t user_read, uint64_t user_write,
                               uint64_t user_except, int commit_empty) {
    struct process *process = process_current();
    if (!process || nfds < 0) return -EINVAL;
    if (nfds > PROCESS_FD_CAPACITY(process)) nfds = PROCESS_FD_CAPACITY(process);
    size_t words = ((size_t)nfds + 63U) / 64U;
    size_t bytes = words * sizeof(uint64_t);
    __attribute__((cleanup(release_bits))) uint64_t *bits =
        (uint64_t *)kmalloc(bytes ? 5 * bytes : 8);
    if (!bits) return -ENOMEM;
    memset(bits, 0, bytes ? 5 * bytes : 8);
    uint64_t *requested_read = bits;
    uint64_t *requested_write = bits + words;
    uint64_t *result_read = bits + 2 * words;
    uint64_t *result_write = bits + 3 * words;
    uint64_t *result_except = bits + 4 * words;
    if (bytes && user_read && copy_from_user(requested_read, user_read, bytes) != 0)
        return -EFAULT;
    if (bytes && user_write && copy_from_user(requested_write, user_write, bytes) != 0)
        return -EFAULT;

    int ready = 0;
    for (int fd = 0; fd < nfds; fd++) {
        int wants_read = bits_test(requested_read, fd);
        int wants_write = bits_test(requested_write, fd);
        if (!wants_read && !wants_write) continue;
        struct file *file = fd_file(fd);
        if (!file) return -EBADF;
        int this_ready = 0;
        if (wants_read && file_read_ready(file) > 0) {
            bits_put(result_read, fd);
            this_ready = 1;
        }
        if (wants_write && file_write_ready(file) > 0) {
            bits_put(result_write, fd);
            this_ready = 1;
        }
        if (this_ready) ready++;
    }
    if (!ready) {
        io_watch_begin(process);
        for (int fd = 0; fd < nfds; fd++) {
            if (bits_test(requested_read, fd)) io_watch_add(process, fd, POLLIN);
            if (bits_test(requested_write, fd)) io_watch_add(process, fd, POLLOUT);
        }
    }
    if ((ready || commit_empty) && bytes) {
        if (user_read && copy_to_user(user_read, result_read, bytes) != 0) return -EFAULT;
        if (user_write && copy_to_user(user_write, result_write, bytes) != 0) return -EFAULT;
        if (user_except && copy_to_user(user_except, result_except, bytes) != 0) return -EFAULT;
    }
    return ready;
}

static size_t process_root_prefix(char *buffer) {
    struct vfs_node *root = process_get_root();
    buffer[0] = '/';
    buffer[1] = '\0';
    if (!root || root == vfs_root) return 1;
    if (vfs_node_path(root, buffer, VFS_PATH_MAX) != 0 || !buffer[0]) {
        buffer[0] = '/';
        buffer[1] = '\0';
        return 1;
    }
    size_t length = strlen(buffer);
    while (length > 1 && buffer[length - 1] == '/') buffer[--length] = '\0';
    return length;
}

static int normalize_path(struct vfs_node *base, const char *input, char *output) {
    if (!input || !input[0]) return -ENOENT;
    size_t floor = process_root_prefix(output);
    VFS_PATH_SCOPED combined = (char *)kmalloc(2 * VFS_PATH_MAX);
    if (!combined) return -ENOMEM;
    size_t at = 0;
    if (input[0] == '/') {
        memcpy(combined, output, floor);
        at = floor;
        if (combined[at - 1] != '/') combined[at++] = '/';
    } else {
        if (vfs_node_path(base ? base : vfs_root, combined, VFS_PATH_MAX) != 0) return -EINVAL;
        at = strlen(combined);
        if (at == 0 || combined[at - 1] != '/') combined[at++] = '/';
    }
    size_t input_length = strlen(input);
    if (at + input_length + 1 > 2 * VFS_PATH_MAX) return -ENAMETOOLONG;
    memcpy(combined + at, input, input_length + 1);

    size_t out = 0;
    output[out++] = '/';
    const char *cursor = combined;
    while (*cursor) {
        while (*cursor == '/') cursor++;
        if (!*cursor) break;
        const char *component = cursor;
        size_t length = 0;
        while (*cursor && *cursor != '/') {
            cursor++;
            if (++length > VFS_NAME_MAX) return -ENAMETOOLONG;
        }
        if (length == 1 && component[0] == '.') continue;
        if (length == 2 && component[0] == '.' && component[1] == '.') {
            if (out > floor) {
                if (output[out - 1] == '/') out--;
                while (out > floor && output[out - 1] != '/') out--;
            }
            continue;
        }
        if (out > 1 && output[out - 1] != '/') output[out++] = '/';
        if (out + length + 1 > VFS_PATH_MAX) return -ENAMETOOLONG;
        memcpy(output + out, component, length);
        out += length;
    }
    if (out > 1 && output[out - 1] == '/') out--;
    if (out < floor) out = floor;
    output[out] = '\0';
    return 0;
}

static struct vfs_node *base_for_dirfd(int dirfd) {
    struct process *process = process_current();
    if (!process) return NULL;
    if (dirfd == AT_FDCWD) return process->cwd;
    if (dirfd < 0 || dirfd >= PROCESS_FD_CAPACITY(process) || !fd_file(dirfd)) return NULL;
    struct file *file = fd_file(dirfd);
    if (file->kind != FILE_KIND_VFS || !file->node || (file->node->flags & 0xFFU) != VFS_DIRECTORY) return NULL;
    return file->node;
}

static int copy_user_path(uint64_t user_path, char *output) {
    int copied = copy_string_from_user(output, VFS_PATH_MAX, user_path);
    if (copied == -2) return -ENAMETOOLONG;
    return copied < 0 ? -EFAULT : 0;
}

static int copy_path_at(int dirfd, uint64_t user_path, char **output) {
    VFS_PATH_SCOPED input = vfs_path_buffer();
    *output = vfs_path_buffer();
    if (!input || !*output) return -ENOMEM;
    int status = copy_user_path(user_path, input);
    if (status != 0) return status;
    struct vfs_node *base = input[0] == '/' ? vfs_root : base_for_dirfd(dirfd);
    if (!base) return -EBADF;
    return normalize_path(base, input, *output);
}

static uint32_t mode_after_umask(uint64_t mode) {
    return ((uint32_t)mode & 07777U) & ~process_get_umask();
}

static int64_t reopen_own_descriptor(const char *path, uint64_t flags) {
    struct process *process = process_current();
    if (!process || !process->files) return -1;

    const char *rest = NULL;
    if (strncmp(path, "/proc/self/fd/", 14) == 0) rest = path + 14;
    else if (strncmp(path, "/proc/thread-self/fd/", 21) == 0) rest = path + 21;
    else if (strncmp(path, "/proc/", 6) == 0) {
        const char *digits = path + 6;
        uint64_t pid = 0;
        while (*digits >= '0' && *digits <= '9') pid = pid * 10 + (uint64_t)(*digits++ - '0');
        if (digits == path + 6 || strncmp(digits, "/fd/", 4) != 0) return -1;
        if (pid != process->pid && pid != process->tgid) return -1;
        rest = digits + 4;
    }
    if (!rest || !*rest) return -1;

    int fd = 0;
    for (const char *at = rest; *at; at++) {
        if (*at < '0' || *at > '9') return -1;
        fd = fd * 10 + (*at - '0');
        if (fd >= PROCESS_FD_CAPACITY(process)) return -1;
    }
    struct file *source = fd_file(fd);
    if (!source || source->kind != FILE_KIND_MEMFD) return -1;

    memfd_ref(source->memfd);
    struct file *file = file_create_memfd(source->memfd,
                                          (uint32_t)(flags & ~O_CLOEXEC));
    if (!file) {
        memfd_destroy(source->memfd);
        return -ENOMEM;
    }
    int installed = process_install_file_flags(process, file, 0,
        (flags & O_CLOEXEC) ? PROCESS_FD_CLOEXEC : 0);
    if (installed < 0) {
        file_unref(file);
        return -EMFILE;
    }
    return installed;
}

static int64_t open_at(int dirfd, uint64_t user_path, uint64_t flags, uint64_t mode) {
    uint64_t supported = O_ACCMODE | O_CREAT | O_EXCL | O_NOCTTY | O_TRUNC |
                         O_APPEND | O_NONBLOCK | O_DSYNC | O_ASYNC | O_DIRECT |
                         O_LARGEFILE | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC | O_NOATIME |
                         O_PATH | O_SYNC;
    if ((flags & O_TMPFILE) == O_TMPFILE) return -EOPNOTSUPP;
    flags &= supported;
    if ((flags & O_PATH) && (flags & (O_CREAT | O_EXCL | O_TRUNC))) return -EINVAL;

    VFS_PATH_SCOPED path = NULL;
    int path_status = copy_path_at(dirfd, user_path, &path);
    if (path_status != 0) return path_status;

    int64_t reopened = reopen_own_descriptor(path, flags);
    if (reopened != -1) return reopened;

    int created = 0;
    struct vfs_node *node = (flags & O_NOFOLLOW) ? vfs_lookup_nofollow(path) : vfs_lookup(path);
    if (!node && (flags & O_CREAT)) {
        if (flags & O_DIRECTORY) return -EINVAL;
        int permitted = cred_may_write_parent(path);
        if (permitted != 0) return permitted;
        node = vfs_create_file_node(path, mode_after_umask(mode));
        if (!node) return -ENOENT;
        created = 1;
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

    struct process *opener = process_current();
    if (created)
        eventfs_emit_file_create(opener->cred.euid, opener->tgid, path);
    else if (!(flags & O_PATH) && (flags & O_TRUNC) && kind == VFS_FILE)
        eventfs_emit_file_write(opener->cred.euid, opener->tgid, path);

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
    struct file *file = process_file_get(process, oldfd);
    if (!file) return -EBADF;
    if (minimum < 0 || (uint64_t)minimum >= process->rlimits[PROCESS_RLIMIT_NOFILE].soft) {
        file_unref(file);
        return -EINVAL;
    }
    int result = process_install_file_flags(process, file, minimum,
        cloexec ? PROCESS_FD_CLOEXEC : 0);
    if (result < 0) {
        file_unref(file);
        return -EMFILE;
    }
    return result;
}

static int64_t sys_dup_to(int oldfd, int newfd, int cloexec, int reject_same) {
    struct process *process = process_current();
    struct file *file = process_file_get(process, oldfd);
    if (!file) return -EBADF;
    int64_t result = newfd;
    struct file *replaced = NULL;
    if (newfd < 0 || (uint64_t)newfd >= process->rlimits[PROCESS_RLIMIT_NOFILE].soft)
        result = -EBADF;
    else if (oldfd == newfd)
        result = reject_same ? -EINVAL : newfd;
    else if (process_install_file_at(process, file, newfd,
                                     cloexec ? PROCESS_FD_CLOEXEC : 0, &replaced) != 0)
        result = -ENOMEM;
    else
        file = NULL;
    if (file) file_unref(file);
    if (replaced) file_unref(replaced);
    return result;
}

static int64_t pipe_size_control(struct file *file, int set, uint64_t wanted) {
    if (!file || (file->kind != FILE_KIND_PIPE_READ && file->kind != FILE_KIND_PIPE_WRITE) ||
        !file->pipe) return -EBADF;
    if (!set) return (int64_t)file->pipe->capacity;
    const struct credentials *cred = cred_current();
    uint64_t ceiling = cred && cred->euid ? PIPE_MAX_CAPACITY : PIPE_ROOT_MAX_CAPACITY;
    if (wanted > ceiling) return -EPERM;
    uint64_t capacity = 4096;
    while (capacity < wanted) capacity *= 2;
    if (capacity < file->pipe->count) return -EBUSY;
    if (pipe_resize(file->pipe, capacity) != 0) return -ENOMEM;
    return (int64_t)capacity;
}

static int64_t sys_pipe(uint64_t user_fds, int flags) {
    if (flags & ~(O_CLOEXEC | O_NONBLOCK)) return -EINVAL;
    struct file *read_end;
    struct file *write_end;
    if (pipe_create(&read_end, &write_end) != 0) return -EMFILE;

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
    struct file *file = fd_file(fd);
    if (!file) return NULL;
    return file->kind == FILE_KIND_SOCKET ? file->socket : NULL;
}

static struct inet_socket *inet_socket_from_fd(int fd) {
    struct file *file = fd_file(fd);
    if (!file) return NULL;
    return file->kind == FILE_KIND_INET_SOCKET ? file->inet_socket : NULL;
}

static struct netlink_socket *netlink_socket_from_fd(int fd) {
    struct file *file = fd_file(fd);
    if (!file) return NULL;
    return file->kind == FILE_KIND_NETLINK_SOCKET ? file->netlink_socket : NULL;
}

static int64_t sys_socket(int domain, int type, int protocol) {
    int base_type = type & 0xF;
    int type_flags = type & ~0xF;
    if (type_flags & ~(SOCK_NONBLOCK | SOCK_CLOEXEC)) return -EINVAL;
    struct process *process = process_current();
    if (domain == TUNIX_AF_UNIX) {
        if ((base_type != TUNIX_SOCK_STREAM && base_type != TUNIX_SOCK_SEQPACKET &&
             base_type != TUNIX_SOCK_DGRAM) || protocol != 0) return -EOPNOTSUPP;
        struct unix_socket *socket =
            unix_socket_create(base_type != TUNIX_SOCK_STREAM);
        if (!socket) return -ENOMEM;

        unix_socket_set_credentials(socket, process ? (int32_t)process->pid : 0,
                                    process ? process->cred.euid : 0,
                                    process ? process->cred.egid : 0);
        struct file *file = file_create_socket(socket);
        if (!file) { unix_socket_unref(socket); return -ENOMEM; }
        file->flags = (uint32_t)(type_flags & SOCK_NONBLOCK);
        return install_new_file(file, type_flags & SOCK_CLOEXEC);
    }
    if (domain == TUNIX_AF_INET || domain == TUNIX_AF_PACKET) {
        if ((domain == TUNIX_AF_PACKET || base_type == TUNIX_SOCK_RAW) && !cred_is_root())
            return -EPERM;
        struct inet_socket *socket = inet_socket_create(domain, base_type, protocol);
        if (!socket) return base_type == TUNIX_SOCK_STREAM ? -EOPNOTSUPP : -EPROTONOSUPPORT;
        struct file *file = file_create_inet_socket(socket);
        if (!file) { inet_socket_unref(socket); return -ENOMEM; }
        file->flags = (uint32_t)(type_flags & SOCK_NONBLOCK);
        return install_new_file(file, type_flags & SOCK_CLOEXEC);
    }
    if (domain == TUNIX_AF_NETLINK) {
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

static int resolve_socket_path(const struct tunix_sockaddr_un *address, char **resolved) {
    char path[sizeof(address->path) + 1];
    memcpy(path, address->path, sizeof(address->path));
    path[sizeof(address->path)] = '\0';
    *resolved = vfs_path_buffer();
    if (!*resolved) return -ENOMEM;
    struct process *process = process_current();
    return normalize_path(path[0] == '/' ? vfs_root : (process ? process->cwd : vfs_root),
                          path, *resolved);
}

static int64_t sys_bind(int fd, uint64_t user_address, uint64_t length) {
    struct unix_socket *unix_value = socket_from_fd(fd);
    if (unix_value) {
        struct tunix_sockaddr_un address;
        int status = copy_sockaddr_un(user_address, length, &address);
        if (status < 0) return status;

        int named = address.path[0] != 0;
        VFS_PATH_SCOPED resolved = NULL;
        if (named) {
            status = resolve_socket_path(&address, &resolved);
            if (status != 0) return status;
            if (vfs_lookup_nofollow(resolved)) return -EADDRINUSE;
        }
        status = unix_socket_bind(unix_value, &address, (size_t)length, resolved);
        if (status == 0 && named) {
            struct process *self = process_current();
            uint32_t mode = 0777U & ~(self ? self->umask : 0U);
            (void)vfs_create_socket_node(resolved, mode);
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
        if (status < 0) return status;
        VFS_PATH_SCOPED resolved = NULL;
        if (address.path[0]) {
            status = resolve_socket_path(&address, &resolved);
            if (status != 0) return status;
        }
        return unix_socket_connect(unix_value, &address, (size_t)length, resolved);
    }
    struct inet_socket *inet_value = inet_socket_from_fd(fd);
    if (!inet_value || !user_address || length < 2 || length > 32) return -EBADF;
    uint8_t address[32];
    if (copy_from_user(address, user_address, (size_t)length) != 0) return -EFAULT;
    struct process *process = process_current();
    return inet_socket_connect(inet_value, address, (size_t)length,
                               process->tgid, process->cred.euid);
}

static int64_t sys_shutdown(int fd, int how) {
    struct file *file = fd_file(fd);
    if (!file) return -EBADF;
    if (file->kind == FILE_KIND_SOCKET) return unix_socket_shutdown(file->socket, how);
    if (file->kind == FILE_KIND_INET_SOCKET) return inet_socket_shutdown(file->inet_socket, how);
    return -ENOTSOCK;
}

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

static void block_and_retry(struct syscall_frame *frame, uint64_t syscall_number,
                            struct file *file, int writing);

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
    int64_t accepted_fd = install_accepted(file, flags, &peer, peer_length,
                                           user_address, user_length);
    if (accepted_fd >= 0) {
        struct process *process = process_current();
        inet_socket_report_accept(accepted, process->tgid, process->cred.euid);
    }
    return accepted_fd;
}

static void accept_or_block(struct syscall_frame *frame, uint64_t syscall_number,
                            int fd, uint64_t user_address, uint64_t user_length,
                            int flags) {
    int64_t result = sys_accept(fd, user_address, user_length, flags);
    struct process *process = process_current();
    struct file *file = process && fd >= 0 && fd < PROCESS_FD_CAPACITY(process)
                            ? fd_file(fd) : NULL;
    if (result == -EAGAIN && file && !(file->flags & O_NONBLOCK)) {
        block_and_retry(frame, syscall_number, file, 0);
        return;
    }
    SYSCALL_RET(frame) = (uint64_t)result;
}

static uint8_t *stage_message(size_t length) {
    return (uint8_t *)kmalloc(length ? length : 1U);
}

static int64_t sys_sendto(int fd, uint64_t user_data, size_t length, int flags,
                          uint64_t user_address, uint64_t address_length) {
    struct unix_socket *unix_value = socket_from_fd(fd);
    if (unix_value) {
        (void)flags;
        (void)user_address;
        (void)address_length;
        if (length > SOCKET_MESSAGE_MAX) length = SOCKET_MESSAGE_MAX;
        uint8_t *data = stage_message(length);
        if (!data) return -ENOMEM;
        int64_t result = length && copy_from_user(data, user_data, length) != 0
                             ? -EFAULT : unix_socket_write(unix_value, length, data);
        kfree(data);
        return result;
    }
    struct netlink_socket *netlink = netlink_socket_from_fd(fd);
    if (netlink) {
        if (length > 4096U) return -EMSGSIZE;
        uint8_t request[4096];
        if (length && copy_from_user(request, user_data, length) != 0) return -EFAULT;

        struct tunix_sockaddr_nl destination;
        int addressed = user_address && address_length >= sizeof(destination) &&
            copy_from_user(&destination, user_address, sizeof(destination)) == 0;
        return netlink_socket_sendto(netlink, request, length, flags,
                                     addressed ? &destination : NULL,
                                     addressed ? sizeof(destination) : 0U);
    }
    struct inet_socket *socket = inet_socket_from_fd(fd);
    if (!socket) return -EBADF;
    if (length > SOCKET_MESSAGE_MAX) {
        if (!inet_socket_is_stream(socket)) return -EMSGSIZE;
        length = SOCKET_MESSAGE_MAX;
    }
    uint8_t address[32];
    const void *address_pointer = NULL;
    if (user_address) {
        if (address_length < 2 || address_length > sizeof(address)) return -EINVAL;
        if (copy_from_user(address, user_address, (size_t)address_length) != 0) return -EFAULT;
        address_pointer = address;
    }
    uint8_t *data = stage_message(length);
    if (!data) return -ENOMEM;
    int64_t result = length && copy_from_user(data, user_data, length) != 0
                         ? -EFAULT
                         : inet_socket_sendto(socket, data, length, flags, address_pointer,
                                              (size_t)address_length);
    kfree(data);
    return result;
}

static int64_t sys_recvfrom(int fd, uint64_t user_data, size_t length, int flags,
                            uint64_t user_address, uint64_t user_address_length) {
    struct unix_socket *unix_value = socket_from_fd(fd);
    if (unix_value) {
        (void)flags;
        if (length > SOCKET_MESSAGE_MAX) length = SOCKET_MESSAGE_MAX;
        uint8_t *data = stage_message(length);
        if (!data) return -ENOMEM;
        int64_t result = unix_socket_read(unix_value, length, data);
        if (result > 0 && copy_to_user(user_data, data, (size_t)result) != 0) result = -EFAULT;
        kfree(data);
        if (result < 0) return result;
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
    if (length > SOCKET_MESSAGE_MAX) length = SOCKET_MESSAGE_MAX;
    uint8_t address[32];
    size_t address_length = sizeof(address);
    if (user_address_length) {
        uint32_t supplied;
        if (copy_from_user(&supplied, user_address_length, sizeof(supplied)) != 0) return -EFAULT;
        address_length = supplied < sizeof(address) ? supplied : sizeof(address);
    }
    uint8_t *data = stage_message(length);
    if (!data) return -ENOMEM;
    int64_t result = inet_socket_recvfrom(socket, data, length, flags,
                                           user_address ? address : NULL,
                                           user_address ? &address_length : NULL);
    if (result > 0 && copy_to_user(user_data, data, (size_t)result) != 0) result = -EFAULT;
    kfree(data);
    if (result < 0) return result;
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
                               size_t capacity, size_t *total, int from_user,
                               int partial) {
    if (!message || !buffer || !total) return -EINVAL;
    if (message->iov_length > UIO_MAXIOV) return -EMSGSIZE;
    size_t completed = 0;
    for (uint64_t index = 0; index < message->iov_length; index++) {
        struct linux_iovec iov;
        if (copy_from_user(&iov, message->iov + index * sizeof(iov), sizeof(iov)) != 0)
            return -EFAULT;
        size_t amount = (size_t)iov.length;
        if (amount > capacity - completed) {
            if (!partial) return -EMSGSIZE;
            amount = capacity - completed;
        }
        if (amount) {
            int status = from_user
                ? copy_from_user(buffer + completed, iov.base, amount)
                : copy_to_user(iov.base, buffer + completed, amount);
            if (status != 0) return -EFAULT;
        }
        completed += amount;
        if (amount < (size_t)iov.length) break;
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
            if (amount > UNIX_MAX_RIGHTS - *file_count) {
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

static size_t message_total_length(const struct linux_msghdr *message) {
    size_t total = 0;
    for (uint64_t index = 0; index < message->iov_length && index < UIO_MAXIOV; index++) {
        struct linux_iovec iov;
        if (copy_from_user(&iov, message->iov + index * sizeof(iov), sizeof(iov)) != 0)
            return total;
        total += (size_t)iov.length;
        if (total > SOCKET_MESSAGE_MAX) return SOCKET_MESSAGE_MAX;
    }
    return total;
}

static int64_t sys_sendmsg(int fd, uint64_t user_message, int flags) {
    if (!user_message) return -EFAULT;
    struct linux_msghdr message;
    if (copy_from_user(&message, user_message, sizeof(message)) != 0) return -EFAULT;
    struct unix_socket *unix_value = socket_from_fd(fd);
    struct inet_socket *stream_socket = unix_value ? NULL : inet_socket_from_fd(fd);
    int partial = (unix_value && !unix_socket_is_seqpacket(unix_value)) ||
                  stream_socket != NULL;
    uint8_t stage[SOCKET_MESSAGE_STAGE];
    uint8_t *data = stage;
    size_t capacity = sizeof(stage);
    if (unix_value) {
        size_t wanted = message_total_length(&message);
        if (wanted > capacity) {
            if (wanted > SOCKET_MESSAGE_MAX) wanted = SOCKET_MESSAGE_MAX;
            uint8_t *large = (uint8_t *)kmalloc(wanted);
            if (large) { data = large; capacity = wanted; }
        }
    }
    size_t length = 0;
    int status = copy_message_iovecs(&message, data, capacity, &length, 1, partial);
    if (status < 0) {
        if (data != stage) kfree(data);
        return status;
    }

    if (unix_value) {
        if (message.name) return -EISDIR;
        struct file *files[UNIX_MAX_RIGHTS] = {0};
        size_t file_count = 0;
        status = collect_scm_rights(&message, files, &file_count);
        if (status < 0) {
            if (data != stage) kfree(data);
            return status;
        }
        if (file_count && length == 0) {
            release_file_array(files, file_count);
            if (data != stage) kfree(data);
            return -EINVAL;
        }
        int64_t result = unix_socket_send_with_rights(unix_value, length, data,
                                                       files, file_count);
        if (result < 0) release_file_array(files, file_count);
        if (data != stage) kfree(data);
        return result;
    }

    struct netlink_socket *netlink = netlink_socket_from_fd(fd);
    if (netlink) {
        if (data != stage) { kfree(data); data = stage; }
        struct tunix_sockaddr_nl destination;
        int addressed = message.name && message.name_length >= sizeof(destination) &&
            copy_from_user(&destination, message.name, sizeof(destination)) == 0;
        return netlink_socket_sendto(netlink, data, length, flags,
                                     addressed ? &destination : NULL,
                                     addressed ? sizeof(destination) : 0U);
    }

    if (!stream_socket) {
        if (data != stage) kfree(data);
        return -EBADF;
    }
    uint8_t address[32];
    const void *address_pointer = NULL;
    if (message.name) {
        if (message.name_length < 2U || message.name_length > sizeof(address)) return -EINVAL;
        if (copy_from_user(address, message.name, message.name_length) != 0) return -EFAULT;
        address_pointer = address;
    }
    int64_t sent = inet_socket_sendto(stream_socket, data, length, flags,
                                      address_pointer,
                                      message.name ? message.name_length : 0U);
    if (data != stage) kfree(data);
    return sent;
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

    int installed[UNIX_MAX_RIGHTS];
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

static int64_t sys_recvmsg(int fd, uint64_t user_message, int flags);

static int64_t sys_recvmmsg(int fd, uint64_t user_vector, unsigned count, int flags) {
    if (!user_vector) return -EFAULT;
    if (count > 1024U) count = 1024U;

    unsigned received = 0;
    for (; received < count; received++) {
        uint64_t element = user_vector + (uint64_t)received * sizeof(struct linux_mmsghdr);
        int64_t result = sys_recvmsg(fd, element,
                                     received ? (flags | MSG_DONTWAIT) : flags);
        if (result < 0) return received ? (int64_t)received : result;
        uint32_t length = (uint32_t)result;
        if (copy_to_user(element + offsetof(struct linux_mmsghdr, msg_len),
                         &length, sizeof(length)) != 0)
            return received ? (int64_t)received : -EFAULT;
        if (flags & MSG_WAITFORONE) flags |= MSG_DONTWAIT;
    }
    return (int64_t)received;
}

static int64_t sys_sendmmsg(int fd, uint64_t user_vector, unsigned count, int flags) {
    if (!user_vector) return -EFAULT;

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
    if (message.iov_length > UIO_MAXIOV) return -EMSGSIZE;

    uint8_t stage[SOCKET_MESSAGE_STAGE];
    uint8_t *data = stage;
    size_t room = sizeof(stage);

    size_t capacity = message_total_length(&message);
    struct unix_socket *unix_value = socket_from_fd(fd);
    if (unix_value && capacity > room) {
        uint8_t *large = (uint8_t *)kmalloc(capacity);
        if (large) { data = large; room = capacity; }
    }
    if (capacity > room) capacity = room;
    if (unix_value) {
        struct file *files[UNIX_MAX_RIGHTS] = {0};
        size_t file_count = 0;
        int64_t result = unix_socket_recv_with_rights(unix_value, capacity, data,
                                                       files, UNIX_MAX_RIGHTS, &file_count);
        if (result < 0) {
            if (data != stage) kfree(data);
            return result;
        }
        if (scatter_message_data(&message, data, (size_t)result) != 0) {
            release_file_array(files, file_count);
            if (data != stage) kfree(data);
            return -EFAULT;
        }
        if (data != stage) { kfree(data); data = stage; }
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
    size_t control_room = message.control_length;
    message.control_length = 0;
    message.flags = 0;
    if (inet_socket_wants_ttl(socket)) {
        size_t hop_length = sizeof(struct linux_cmsghdr) + sizeof(int);
        if (message.control && control_room >= cmsg_align(hop_length)) {
            struct linux_cmsghdr header = {hop_length, IPPROTO_IP_LEVEL, IP_TTL_OPTION};
            int hops = inet_socket_last_ttl(socket);
            if (copy_to_user(message.control, &header, sizeof(header)) != 0 ||
                copy_to_user(message.control + sizeof(header), &hops, sizeof(hops)) != 0)
                return -EFAULT;
            message.control_length = hop_length;
        } else {
            message.flags |= MSG_CTRUNC;
        }
    }
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
        if (level == SOL_SOCKET && option == SO_PASSCRED && length >= sizeof(int32_t)) {
            int32_t enabled;
            if (copy_from_user(&enabled, user_value, sizeof(enabled)) != 0) return -EFAULT;
            netlink_socket_set_passcred(netlink_option, enabled != 0);
            return 0;
        }

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
        if (option == SO_TYPE || option == SO_ERROR || option == SO_ACCEPTCONN ||
            option == SO_SNDBUF || option == SO_RCVBUF) {
            if (supplied < sizeof(int32_t)) return -EINVAL;
            int32_t value = option == SO_TYPE ? (unix_socket_is_seqpacket(unix_value)
                                                 ? TUNIX_SOCK_SEQPACKET : TUNIX_SOCK_STREAM) :
                (option == SO_ACCEPTCONN ? unix_socket_is_listener(unix_value) :
                 ((option == SO_SNDBUF || option == SO_RCVBUF) ? (int32_t)PIPE_CAPACITY : 0));
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
    struct file *file = fd_file(fd);
    if (!file) return -EBADF;

    if (file->kind == FILE_KIND_MEMFD)
        return memfd_truncate(file->memfd, length) == 0 ? 0 : -ENOMEM;
    if (file->kind != FILE_KIND_VFS || !file->node) return -EINVAL;
    return vfs_truncate(file->node, length) == 0 ? 0 : -EIO;
}

static int64_t sys_fallocate(int fd, int mode, uint64_t offset, uint64_t length) {
    struct process *process = process_current();
    if (!process || fd < 0 || fd >= PROCESS_FD_CAPACITY(process) || !fd_file(fd)) return -EBADF;
    if (mode != 0) return -EOPNOTSUPP;
    if ((int64_t)offset < 0 || (int64_t)length < 0) return -EINVAL;
    if (length > UINT64_MAX - offset) return -EFBIG;

    struct file *file = fd_file(fd);
    uint64_t needed = offset + length;

    if (file->kind == FILE_KIND_MEMFD) {
        if (needed <= memfd_size(file->memfd)) return 0;
        return memfd_truncate(file->memfd, needed) == 0 ? 0 : -ENOSPC;
    }
    if (file->kind == FILE_KIND_VFS && file->node) {
        if (needed <= file->node->length) return 0;
        return vfs_truncate(file->node, needed) == 0 ? 0 : -ENOSPC;
    }
    return -ENODEV;
}

static int64_t sys_faccess_at(int dirfd, uint64_t user_path, int mode, int flags) {
    if (flags & ~(AT_EACCESS | AT_SYMLINK_NOFOLLOW)) return -EINVAL;
    if (mode & ~7) return -EINVAL;
    VFS_PATH_SCOPED path = NULL;
    int status = copy_path_at(dirfd, user_path, &path);
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
    __attribute__((cleanup(release_bits))) uint64_t *groups =
        (uint64_t *)kmalloc((size_t)size * sizeof(uint32_t) + 8U);
    if (!groups) return -ENOMEM;
    if (size && copy_from_user(groups, user_list, (size_t)size * sizeof(uint32_t)) != 0)
        return -EFAULT;
    return cred_set_groups((uint32_t)size, (const uint32_t *)groups);
}

static int64_t sys_flock(int fd, int operation) {
    struct process *process = process_current();
    if (!process || fd < 0 || fd >= PROCESS_FD_CAPACITY(process) || !fd_file(fd)) return -EBADF;
    return file_flock(fd_file(fd), operation);
}

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
    struct file *file = fd_file(fd);
    if (!file) return -EBADF;
    if (file->kind != FILE_KIND_VFS || !file->node) return -EINVAL;

    struct linux_flock lock;
    if (copy_from_user(&lock, user_lock, sizeof(lock)) != 0) return -EFAULT;

    if (command == F_GETLK) {
        if (file->node->posix_lock_pid && file->node->posix_lock_pid != process->tgid) {
            lock.type = file->node->posix_lock_write ? F_WRLCK : F_RDLCK;
            lock.pid = (int32_t)file->node->posix_lock_pid;
        } else {
            lock.type = F_UNLCK;
        }
        return copy_to_user(user_lock, &lock, sizeof(lock)) == 0 ? 0 : -EFAULT;
    }

    return vfs_posix_lock(file->node, lock.type, process->tgid);
}

static int64_t sys_fsync(int fd) {
    struct file *file = fd_file(fd);
    if (!file) return -EBADF;
    if (file->kind == FILE_KIND_FRAMEBUFFER) return 0;
    if (file->kind != FILE_KIND_VFS || !file->node) return -EINVAL;
    uint32_t node_type = file->node->flags & 0xFFU;
    if (node_type == VFS_FILE || node_type == VFS_DIRECTORY || node_type == VFS_BLOCKDEVICE) {
        vfs_flush_mapped(file->node);
        if (ext2fs_owns(file->node) && vfs_fsync(file->node) != 0) return -EIO;
        return 0;
    }
    return -EINVAL;
}

static int64_t sys_ioctl(int fd, unsigned long request, uint64_t user_argument) {
    struct process *process = process_current();
    if (!process || fd < 0 || fd >= PROCESS_FD_CAPACITY(process) || !fd_file(fd)) return -EBADF;

    request &= 0xFFFFFFFFUL;
    struct file *file = fd_file(fd);

    if (request == FIONBIO) {
        int32_t enabled;
        if (!user_argument ||
            copy_from_user(&enabled, user_argument, sizeof(enabled)) != 0) return -EFAULT;
        if (enabled) file->flags |= (uint32_t)O_NONBLOCK;
        else file->flags &= ~(uint32_t)O_NONBLOCK;
        return 0;
    }
    if (request == FIONREAD) {
        if (!user_argument) return -EFAULT;
        int32_t available = 0;
        switch (file->kind) {
        case FILE_KIND_SOCKET:
            available = (int32_t)unix_socket_read_available(file->socket);
            break;
        case FILE_KIND_PIPE_READ:
            available = file->pipe ? (int32_t)file->pipe->count : 0;
            break;
        case FILE_KIND_VFS:
            if (!file->node || (file->node->flags & 0xFFU) != VFS_FILE) return -ENOTTY;
            available = file->node->length > file->offset
                ? (int32_t)(file->node->length - file->offset) : 0;
            break;
        default:
            return -ENOTTY;
        }
        return copy_to_user(user_argument, &available, sizeof(available)) == 0 ? 0 : -EFAULT;
    }
    if (file->kind == FILE_KIND_INET_SOCKET && request == SIOCGIFCONF) {
        if (!user_argument) return -EFAULT;
        struct linux_ifconf ifconf;
        if (copy_from_user(&ifconf, user_argument, sizeof(ifconf)) != 0) return -EFAULT;
        if (ifconf.length < 0) return -EINVAL;

        struct linux_ifreq ifreq;
        memset(&ifreq, 0, sizeof(ifreq));
        memcpy(ifreq.name, "eth0", 5);
        ifreq.value[0] = 2;
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
        if (((request >> 8) & 0xFFU) == (unsigned)'E')
            return input_reader_ioctl(file->input_reader,
                                      (unsigned)(uintptr_t)file->node->data,
                                      request, user_argument);
        if (file->node->file_ioctl)
            return file->node->file_ioctl(file, request, user_argument);
        if (file->node->ioctl)
            return file->node->ioctl(file->node, request, user_argument);
    }
    if (file->kind == FILE_KIND_FRAMEBUFFER)
        return framebuffer_file_ioctl(file, request, user_argument);

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
    if (file->node->file_ioctl)
        return file->node->file_ioctl(file, request, user_argument);
    if (file->node->ioctl) return file->node->ioctl(file->node, request, user_argument);

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

    stat->st_mode = type | (node->mode & 07777U);
    if (kind == VFS_CHARDEVICE || kind == VFS_BLOCKDEVICE) {
        uint64_t major = node->dev_major;
        uint64_t minor = node->dev_minor;
        stat->st_rdev = ((major & 0xFFFULL) << 8) | (minor & 0xFFULL) |
                        ((major & ~0xFFFULL) << 32) | ((minor & ~0xFFULL) << 12);
    }

    stat->st_atim.tv_sec = (int64_t)node->atime;
    stat->st_mtim.tv_sec = (int64_t)node->mtime;
    stat->st_ctim.tv_sec = (int64_t)node->ctime;
}

static int64_t stat_path(int dirfd, uint64_t user_path, uint64_t user_stat, int follow) {
    VFS_PATH_SCOPED path = NULL;
    int status = copy_path_at(dirfd, user_path, &path);
    if (status != 0) return status;
    struct vfs_node *node = follow ? vfs_lookup(path) : vfs_lookup_nofollow(path);
    if (!node) return -ENOENT;
    struct linux_stat stat;
    fill_stat(node, &stat);
    return copy_to_user(user_stat, &stat, sizeof(stat)) == 0 ? 0 : -EFAULT;
}

static void fill_statfs(struct vfs_node *node, struct linux_statfs *out) {
    memset(out, 0, sizeof(*out));

    out->f_namelen = VFS_NAME_MAX;

    struct vfs_node *volatile_root = NULL;
    for (struct vfs_node *walk = node; walk; walk = walk->parent) {
        if (walk->flags & VFS_VOLATILE) volatile_root = walk;

        if (walk->parent == walk) break;
    }

    if (volatile_root) {
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
    if (ext2fs_stats(node, &stats) == 0) {
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

    out->f_type = TMPFS_MAGIC;
    out->f_bsize = PMM_PAGE_SIZE;
    out->f_frsize = PMM_PAGE_SIZE;
    out->f_blocks = pmm_usable_page_count();
    out->f_bfree = pmm_free_page_count();
    out->f_bavail = out->f_bfree;
}

static int64_t sys_statfs(uint64_t user_path, uint64_t user_buf) {
    VFS_PATH_SCOPED path = NULL;
    int status = copy_path_at(AT_FDCWD, user_path, &path);
    if (status != 0) return status;
    struct vfs_node *node = vfs_lookup(path);
    if (!node) return -ENOENT;
    struct linux_statfs out;
    fill_statfs(node, &out);
    return copy_to_user(user_buf, &out, sizeof(out)) == 0 ? 0 : -EFAULT;
}

static int64_t sys_fstatfs(int fd, uint64_t user_buf) {
    struct file *file = fd_file(fd);
    if (!file) return -EBADF;
    if ((file->kind != FILE_KIND_VFS && file->kind != FILE_KIND_EVENTFS) ||
        !file->node) return -EBADF;
    struct linux_statfs out;
    fill_statfs(file->node, &out);
    return copy_to_user(user_buf, &out, sizeof(out)) == 0 ? 0 : -EFAULT;
}

static int fill_stat_nodeless(struct file *file, struct linux_stat *stat) {
    uint32_t type;
    uint64_t size = 0;
    switch (file->kind) {
        case FILE_KIND_PIPE_READ:
        case FILE_KIND_PIPE_WRITE:
            type = 0010000U;
            break;
        case FILE_KIND_SOCKET:
        case FILE_KIND_INET_SOCKET:
        case FILE_KIND_NETLINK_SOCKET:
            type = 0140000U;
            break;
        case FILE_KIND_MEMFD:
            type = 0100000U;
            size = memfd_size(file->memfd);
            break;
        default:
            return -1;
    }
    memset(stat, 0, sizeof(*stat));
    stat->st_mode = type | 0600U;
    stat->st_nlink = 1;
    stat->st_blksize = 4096;
    stat->st_size = (int64_t)size;
    stat->st_blocks = (int64_t)((size + 511U) / 512U);

    stat->st_ino = (uint64_t)(uintptr_t)file;
    return 0;
}

static int stat_from_file(struct file *file, struct linux_stat *stat) {
    if (!file) return -1;
    if (file->node &&
        (file->kind == FILE_KIND_VFS || file->kind == FILE_KIND_PTY_MASTER ||
         file->kind == FILE_KIND_PTY_SLAVE || file->kind == FILE_KIND_INPUT ||
         file->kind == FILE_KIND_FRAMEBUFFER || file->kind == FILE_KIND_EVENTFS)) {
        fill_stat(file->node, stat);
        return 0;
    }
    return fill_stat_nodeless(file, stat);
}

static int64_t sys_fstat(int fd, uint64_t user_stat) {
    struct process *process = process_current();
    if (!process || fd < 0 || fd >= PROCESS_FD_CAPACITY(process) || !fd_file(fd)) return -EBADF;
    struct linux_stat stat;
    if (stat_from_file(fd_file(fd), &stat) != 0) return -EBADF;
    return copy_to_user(user_stat, &stat, sizeof(stat)) == 0 ? 0 : -EFAULT;
}

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

    out->stx_rdev_major = (uint32_t)(((basic.st_rdev >> 8) & 0xFFFU) |
                                     ((basic.st_rdev >> 32) & ~0xFFFU));
    out->stx_rdev_minor = (uint32_t)((basic.st_rdev & 0xFFU) |
                                     ((basic.st_rdev >> 12) & ~0xFFU));
}

static int64_t sys_statx(int dirfd, uint64_t user_path, int flags,
                         uint32_t mask, uint64_t user_buf) {
    (void)mask;
    if (flags & ~(AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH | AT_NO_AUTOMOUNT |
                  AT_STATX_SYNC_TYPE))
        return -EINVAL;

    char first = 0;
    if (copy_from_user(&first, user_path, 1) != 0) return -EFAULT;

    struct linux_stat basic;
    if (!first && (flags & AT_EMPTY_PATH) && dirfd >= 0) {
        struct process *process = process_current();
        if (!process || dirfd >= PROCESS_FD_CAPACITY(process) || !fd_file(dirfd)) return -EBADF;
        if (stat_from_file(fd_file(dirfd), &basic) != 0) return -EBADF;
    } else {
        VFS_PATH_SCOPED path = NULL;
        int status = copy_path_at(dirfd, user_path, &path);
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
    struct file *file = fd_file(fd);
    if (!file) return -EBADF;
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
    struct file *file = fd_file(fd);
    if (!file) return -EBADF;
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
        uint8_t record[(19 + VFS_NAME_MAX + 1 + 7) & ~7];
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
    VFS_PATH_SCOPED path = vfs_path_buffer();
    VFS_PATH_SCOPED prefix = vfs_path_buffer();
    if (!path || !prefix) return -ENOMEM;
    if (vfs_node_path(process->cwd, path, VFS_PATH_MAX) != 0) return -ENAMETOOLONG;
    size_t floor = process_root_prefix(prefix);
    const char *visible = path;
    if (floor > 1 && strncmp(path, prefix, floor) == 0)
        visible = path[floor] ? path + floor : "/";
    size_t length = strlen(visible) + 1;
    if (length > size) return -ERANGE;
    return copy_to_user(user_buffer, visible, length) == 0 ? (int64_t)length : -EFAULT;
}

static int64_t xattr_target_exists(uint64_t user_path, int follow) {
    VFS_PATH_SCOPED path = NULL;
    int status = copy_path_at(AT_FDCWD, user_path, &path);
    if (status != 0) return status;
    struct vfs_node *node = follow ? vfs_lookup(path) : vfs_lookup_nofollow(path);
    return node ? 0 : -ENOENT;
}

static int64_t xattr_descriptor_exists(int fd) {
    struct process *process = process_current();
    if (!process || !process->files || fd < 0 || fd >= PROCESS_FD_CAPACITY(process)) return -EBADF;
    return fd_file(fd) ? 0 : -EBADF;
}

static int64_t sys_chroot(uint64_t user_path) {
    struct process *process = process_current();
    if (!process) return -EINVAL;
    if (process->cred.euid != 0) return -EPERM;
    VFS_PATH_SCOPED path = NULL;
    int status = copy_path_at(AT_FDCWD, user_path, &path);
    if (status != 0) return status;
    struct vfs_node *node = vfs_lookup(path);
    if (!node) return -ENOENT;
    if ((node->flags & 0xFFU) != VFS_DIRECTORY) return -ENOTDIR;
    int permitted = cred_may_path(path, node, CRED_EXEC);
    if (permitted != 0) return permitted;
    process_set_root(node);
    struct vfs_node *previous = process->cwd;
    vfs_node_ref(node);
    process->cwd = node;
    vfs_node_unref(previous);
    return 0;
}

static void set_cwd(struct process *process, struct vfs_node *node) {
    struct vfs_node *previous = process->cwd;
    vfs_node_ref(node);
    process->cwd = node;
    vfs_node_unref(previous);
}

static int64_t sys_chdir(uint64_t user_path) {
    VFS_PATH_SCOPED path = NULL;
    int status = copy_path_at(AT_FDCWD, user_path, &path);
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
    struct file *file = fd_file(fd);
    if (!file) return -EBADF;
    if (file->kind != FILE_KIND_VFS || !file->node || (file->node->flags & 0xFFU) != VFS_DIRECTORY) return -ENOTDIR;
    set_cwd(process, file->node);
    return 0;
}

static int64_t sys_mkdir_at(int dirfd, uint64_t user_path, uint64_t mode) {
    VFS_PATH_SCOPED path = NULL;
    int status = copy_path_at(dirfd, user_path, &path);
    if (status != 0) return status;
    if (vfs_lookup(path)) return -EEXIST;
    int permitted = cred_may_write_parent(path);
    if (permitted != 0) return permitted;
    return vfs_create_directory(path, mode_after_umask(mode)) ? 0 : -ENOENT;
}

static int64_t sys_readlink_at(int dirfd, uint64_t user_path, uint64_t user_buffer, size_t size) {
    VFS_PATH_SCOPED path = NULL;
    int status = copy_path_at(dirfd, user_path, &path);
    if (status != 0) return status;
    const char *special_target = NULL;
    if (strcmp(path, "/proc/self/exe") == 0) special_target = process_current()->exe_path ? process_current()->exe_path : "";
    else if (strcmp(path, "/proc/thread-self/exe") == 0) special_target = process_current()->exe_path ? process_current()->exe_path : "";
    if (special_target) {
        size_t length = strlen(special_target);
        if (length > size) length = size;
        return copy_to_user(user_buffer, special_target, length) == 0 ? (int64_t)length : -EFAULT;
    }

    struct vfs_node *node = vfs_lookup_nofollow(path);
    if (!node) return -ENOENT;
    if ((node->flags & 0xFFU) != VFS_SYMLINK) return -EINVAL;
    int64_t length = (int64_t)node->length;
    if (!node->data || length < 0) return -EINVAL;
    if ((size_t)length > size) length = (int64_t)size;
    return copy_to_user(user_buffer, node->data, (size_t)length) == 0 ? length : -EFAULT;
}

static int64_t sys_unlink_at(int dirfd, uint64_t user_path, int flags) {
    if (flags & ~AT_REMOVEDIR) return -EINVAL;
    VFS_PATH_SCOPED path = NULL;
    int status = copy_path_at(dirfd, user_path, &path);
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
    if (vfs_remove(path, (flags & AT_REMOVEDIR) != 0) != 0) return -EIO;
    struct process *process = process_current();
    eventfs_emit_file_remove(process->cred.euid, process->tgid, path);
    return 0;
}

static int64_t sys_rename_at(int old_dirfd, uint64_t user_old_path,
                             int new_dirfd, uint64_t user_new_path,
                             unsigned flags) {
    if (flags != 0) return -EINVAL;
    VFS_PATH_SCOPED old_path = NULL;
    VFS_PATH_SCOPED new_path = NULL;
    int status = copy_path_at(old_dirfd, user_old_path, &old_path);
    if (status != 0) return status;
    status = copy_path_at(new_dirfd, user_new_path, &new_path);
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
    if (vfs_rename(old_path, new_path) != 0) return -EIO;
    struct process *process = process_current();
    eventfs_emit_file_rename(process->cred.euid, process->tgid,
                             old_path, new_path);
    return 0;
}

static int64_t sys_link_at(int old_dirfd, uint64_t user_old_path,
                           int new_dirfd, uint64_t user_new_path, int flags) {
    if (flags & ~AT_SYMLINK_FOLLOW) return -EINVAL;
    VFS_PATH_SCOPED old_path = NULL;
    VFS_PATH_SCOPED new_path = NULL;
    int status = copy_path_at(old_dirfd, user_old_path, &old_path);
    if (status != 0) return status;
    status = copy_path_at(new_dirfd, user_new_path, &new_path);
    if (status != 0) return status;

    struct vfs_node *node = (flags & AT_SYMLINK_FOLLOW) ? vfs_lookup(old_path)
                                                        : vfs_lookup_nofollow(old_path);
    if (!node) return -ENOENT;

    if ((node->flags & 0xFFU) == VFS_DIRECTORY) return -EPERM;
    if (node->flags & VFS_READONLY) return -EROFS;
    if (vfs_lookup_nofollow(new_path)) return -EEXIST;
    int permitted = cred_may_write_parent(new_path);
    if (permitted != 0) return permitted;
    return vfs_link(node, new_path) == 0 ? 0 : -EIO;
}

static int64_t sys_mount(uint64_t user_source, uint64_t user_target,
                         uint64_t user_type, uint64_t flags, uint64_t user_data) {
    (void)user_data;
    const struct credentials *cred = cred_current();
    if (cred && cred->euid != 0) return -EPERM;
    if (flags > 0xFFFFFFFFULL) return -EINVAL;

    VFS_PATH_SCOPED source = vfs_path_buffer();
    VFS_PATH_SCOPED target = vfs_path_buffer();
    char type[64];
    if (!source || !target) return -ENOMEM;
    source[0] = '\0';
    type[0] = '\0';
    if (user_source && copy_user_path(user_source, source) != 0) return -EFAULT;
    int status = copy_user_path(user_target, target);
    if (status != 0) return status;
    if (user_type && copy_string_from_user(type, sizeof(type), user_type) < 0)
        return -EFAULT;
    return vfs_mount(user_source ? source : NULL, target,
                     user_type ? type : "", (uint32_t)flags);
}

static int64_t sys_umount2(uint64_t user_target, int flags) {
    (void)flags;
    const struct credentials *cred = cred_current();
    if (cred && cred->euid != 0) return -EPERM;
    VFS_PATH_SCOPED target = vfs_path_buffer();
    if (!target) return -ENOMEM;
    int status = copy_user_path(user_target, target);
    if (status != 0) return status;
    return vfs_umount(target);
}

static int64_t sys_mknodat(int dirfd, uint64_t user_path, uint32_t mode,
                           uint64_t device) {
    (void)device;
    VFS_PATH_SCOPED path = NULL;
    int status = copy_path_at(dirfd, user_path, &path);
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
    VFS_PATH_SCOPED target = vfs_path_buffer();
    VFS_PATH_SCOPED link_path = NULL;
    if (!target) return -ENOMEM;
    int status = copy_user_path(user_target, target);
    if (status != 0) return status;
    if (!target[0]) return -ENOENT;
    status = copy_path_at(new_dirfd, user_link_path, &link_path);
    if (status != 0) return status;
    if (vfs_lookup_nofollow(link_path)) return -EEXIST;
    int permitted = cred_may_write_parent(link_path);
    if (permitted != 0) return permitted;
    return vfs_create_symlink(link_path, target, 0) ? 0 : -EIO;
}

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
    VFS_PATH_SCOPED path = NULL;
    int status = copy_path_at(dirfd, user_path, &path);
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
    VFS_PATH_SCOPED path = NULL;
    int status = copy_path_at(dirfd, user_path, &path);
    if (status != 0) return status;
    struct vfs_node *node = (flags & AT_SYMLINK_NOFOLLOW) ? vfs_lookup_nofollow(path) : vfs_lookup(path);
    if (!node) return -ENOENT;
    int permitted = cred_may_search(path);
    if (permitted != 0) return permitted;
    return change_owner(node, uid, gid);
}

static int64_t sys_fchown(int fd, uint32_t uid, uint32_t gid) {
    struct file *file = fd_file(fd);
    if (!file) return -EBADF;
    if (file->kind != FILE_KIND_VFS || !file->node) return 0;
    return change_owner(file->node, uid, gid);
}

static int64_t sys_fchmod(int fd, uint32_t mode) {
    struct file *file = fd_file(fd);
    if (!file) return -EBADF;
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
        struct process *process = process_current();
        if (!process || dirfd < 0 || dirfd >= PROCESS_FD_CAPACITY(process) || !fd_file(dirfd))
            return -EBADF;
        struct file *file = fd_file(dirfd);
        if (file->kind != FILE_KIND_VFS || !file->node) return -EBADF;
        node = file->node;
    } else {
        VFS_PATH_SCOPED path = NULL;
        int status = copy_path_at(dirfd, user_path, &path);
        if (status != 0) return status;
        node = (flags & AT_SYMLINK_NOFOLLOW) ? vfs_lookup_nofollow(path)
                                             : vfs_lookup(path);
        if (!node) return -ENOENT;
    }
    if (node->flags & VFS_READONLY) return -EROFS;

    if (!cred_owns(node) && (user_times || cred_may(node, CRED_WRITE) != 0))
        return -EPERM;

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

static int map_shared_object(struct process *process, uint64_t start,
                             uint64_t end, struct memfd_object *object,
                             uint64_t file_offset, uint64_t flags, int private) {
    uint64_t extra = PAGE_SHARED | PAGE_FILEBACKED;
    if (private) {
        extra = PAGE_FILEBACKED | ((flags & PAGE_WRITE) ? PAGE_COW : 0);
        flags &= ~PAGE_WRITE;
    }
    for (uint64_t address = start; address < end; address += 4096) {
        uint64_t index = (file_offset + (address - start)) / 4096ULL;
        uint64_t physical = memfd_page(object, index);

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
    process_unmap_area(start, end);

    vmm_flush_batch_begin();
    for (uint64_t address = start; address < end; address += 4096) {
        uint64_t physical;
        uint64_t flags;
        if (vmm_translate(process->cr3, address, &physical, &flags) == 0) {
            vmm_unmap_page_in(process->cr3, address);
            if (!(flags & PAGE_DEVICE))
                vmm_free_page_after_flush(physical & ~0xFFFULL);
        }
    }
    vmm_prune_empty_tables(process->cr3, start, end);
    vmm_flush_batch_end();
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
    if (!process_area_range_free(base, base + length)) return 0;
    for (uint64_t page = base; page < base + length; page += 4096) {
        if (vmm_translate(process->cr3, page, NULL, NULL) == 0) return 0;
    }
    return 1;
}

static int find_mapping_range(struct process *process, uint64_t start,
                              uint64_t length, uint64_t *base_out) {
    uint64_t base = align_up(start, 4096);
    uint64_t ceiling = process_stack_floor(process) - 4096ULL;
    while (base < ceiling && length <= ceiling - base) {
        uint64_t candidate;
        if (process_find_free_range(base, length, &candidate) != 0) return -1;
        if (candidate > ceiling || length > ceiling - candidate) break;
        if (mapping_range_free(process, candidate, length)) {
            *base_out = candidate;
            return 0;
        }
        if (ceiling - candidate < length + 4096ULL) break;
        base = candidate + 4096;
    }
    return -1;
}

static int64_t map_device(struct process *process, struct file *file, uint64_t base,
                          uint64_t length, uint64_t offset, uint64_t page_flags,
                          int advance_mmap_base) {
    if (process_map_area(base, base + length, page_flags | PAGE_SHARED, VM_DEVICE,
                         file, offset) != 0) return -ENOMEM;
    if (advance_mmap_base) {
        process->mmap_base = base + length + 4096;
        if (process->memory) process->memory->mmap_base = process->mmap_base;
    }
    process_memory_leave();
    int64_t status = file->kind == FILE_KIND_DMABUF
        ? drm_dmabuf_mmap(file, process->cr3, base, length, offset, page_flags | PAGE_SHARED)
        : file->node->mmap(file->node, file, process->cr3, base, length, offset,
                           page_flags | PAGE_SHARED);
    process_memory_enter();
    if (status < 0) {
        unmap_pages(process, base, base + length);
        return status;
    }
    return (int64_t)base;
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
        if (fd < 0 || fd >= PROCESS_FD_CAPACITY(process) || !fd_file(fd)) return -EBADF;
        file = fd_file(fd);

        if (file->kind == FILE_KIND_MEMFD) {
            if (map_shared_object(process, base, base + length, file->memfd,
                                  offset, page_flags,
                                  (flags & MAP_PRIVATE) != 0) != 0) {
                unmap_pages(process, base, base + length);
                return -ENOMEM;
            }

            (void)process_map_area(base, base + length, page_flags,
                                   VM_MEMFD | ((flags & MAP_PRIVATE) ? VM_PRIVATE : 0),
                                   file, offset);
            if (advance_mmap_base) {
                process->mmap_base = base + length + 4096;
                if (process->memory) process->memory->mmap_base = process->mmap_base;
            }
            return (int64_t)base;
        }

        if (file->kind == FILE_KIND_DMABUF) {
            if (!(flags & MAP_SHARED)) return -EINVAL;
            return map_device(process, file, base, length, offset, page_flags,
                              advance_mmap_base);
        }
        if ((file->kind != FILE_KIND_VFS && file->kind != FILE_KIND_FRAMEBUFFER) ||
            !file->node) return -ENODEV;
        if (file->node->mmap) {
            if (!(flags & MAP_SHARED)) return -EINVAL;
            return map_device(process, file, base, length, offset, page_flags,
                              advance_mmap_base);
        }
    }

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

    if (file && file->kind == FILE_KIND_VFS && file->node &&
        (file->node->flags & 0xFFU) == VFS_FILE) {
        if ((flags & MAP_SHARED) && (prot & PROT_WRITE) &&
            (file->flags & O_ACCMODE) == O_RDONLY) return -EACCES;
        uint32_t kind = VM_FILE_PAGES | ((flags & MAP_PRIVATE) ? VM_PRIVATE : 0);
        if (process_map_area(base, base + length, page_flags, kind, file, offset) != 0)
            return -ENOMEM;
        if (advance_mmap_base) {
            process->mmap_base = base + length + 4096;
            if (process->memory) process->memory->mmap_base = process->mmap_base;
        }
        return (int64_t)base;
    }

    uint64_t allocation_flags = file ? (page_flags | PAGE_WRITE) : page_flags;

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

        if (file->kind == FILE_KIND_VFS) vfs_release_data(file->node);
    }
    if (file && !(prot & PROT_WRITE)) {
        uint64_t final_flags = PAGE_USER | PAGE_PRESENT | page_flags;
        int failed = 0;
        vmm_flush_batch_begin();
        for (uint64_t page = base; page < base + length; page += 4096) {
            if (vmm_protect_page_in(process->cr3, page, final_flags) != 0) {
                failed = 1;
                break;
            }
        }
        vmm_flush_batch_end();
        if (failed) {
            unmap_pages(process, base, base + length);
            return -ENOMEM;
        }
    }
    if (advance_mmap_base) {
        process->mmap_base = base + length + 4096;
        if (process->memory) process->memory->mmap_base = process->mmap_base;
    }
    return (int64_t)base;
}

static int64_t sys_msync(uint64_t address, uint64_t length, int flags) {
    struct process *process = process_current();
    if (!process || (address & 0xFFFULL)) return -EINVAL;
    if (flags & ~(MS_ASYNC | MS_INVALIDATE | MS_SYNC)) return -EINVAL;
    if ((flags & MS_ASYNC) && (flags & MS_SYNC)) return -EINVAL;
    if (!length) return 0;
    length = align_up(length, 4096);
    if (address >= USER_ADDRESS_LIMIT || length > USER_ADDRESS_LIMIT - address)
        return -ENOMEM;
    if (!process_sync_file_areas(address, address + length)) return -ENOMEM;
    if ((flags & MS_SYNC) && vfs_sync() != 0) return -EIO;
    return 0;
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
    if (process_map_area(base, base + length, page_flags, VM_MEMFD, file, 0) != 0) {
        unmap_pages(process, base, base + length);
        file_unref(file);
        return -ENOMEM;
    }

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

    sysvshm_reap();
    return 0;
}

static int64_t sys_shmctl(int id, int command, uint64_t user_buffer) {
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

static int64_t mremap_backed(struct process *process, uint64_t address,
                             uint64_t old_length, uint64_t new_length,
                             int flags, struct file *backing,
                             uint64_t backing_offset, uint32_t kind,
                             uint64_t area_flags) {
    int lazy = backing && (kind & VM_FILE_PAGES);
    if (new_length < old_length) {
        unmap_pages(process, address + new_length, address + old_length);

        if (backing)
            (void)process_map_area(address, address + new_length, area_flags,
                                   kind, backing, backing_offset);
        return (int64_t)address;
    }

    uint64_t tail = address + old_length;
    uint64_t extra = new_length - old_length;
    if (mapping_range_free(process, tail, extra)) {
        int ok;
        if (lazy) {
            ok = 1;
        } else if (backing && backing->kind == FILE_KIND_MEMFD) {
            ok = map_shared_object(process, tail, tail + extra, backing->memfd,
                                   backing_offset + old_length,
                                   area_flags, (kind & VM_PRIVATE) != 0) == 0;
        } else if (!backing) {
            ok = map_zero_pages(process, tail, tail + extra, PAGE_WRITE) == 0;
        } else {
            ok = 0;
        }
        if (ok) {
            if (backing)
                (void)process_map_area(address, address + new_length, area_flags,
                                       kind, backing, backing_offset);
            return (int64_t)address;
        }
        unmap_pages(process, tail, tail + extra);
    }

    if (!(flags & MREMAP_MAYMOVE)) return -ENOMEM;

    int movable = backing && (backing->kind == FILE_KIND_MEMFD ||
                              (lazy && !(kind & VM_PRIVATE)));
    if (!movable) return -ENOMEM;

    uint64_t destination;
    uint64_t search_start = process->memory ? process->memory->mmap_base :
                                              process->mmap_base;
    if (find_mapping_range(process, search_start, new_length, &destination) != 0)
        return -ENOMEM;

    if (!lazy &&
        map_shared_object(process, destination, destination + new_length,
                          backing->memfd, backing_offset, area_flags,
                          (kind & VM_PRIVATE) != 0) != 0) {
        unmap_pages(process, destination, destination + new_length);
        return -ENOMEM;
    }

    unmap_pages(process, address, address + old_length);
    (void)process_map_area(destination, destination + new_length, area_flags,
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

    if (flags & ~MREMAP_MAYMOVE) return -EINVAL;
    (void)new_address;

    old_length = align_up(old_length, 4096);
    new_length = align_up(new_length, 4096);
    if (address >= USER_ADDRESS_LIMIT ||
        new_length > USER_ADDRESS_LIMIT - address) return -EINVAL;
    if (vmm_translate(process->cr3, address, NULL, NULL) != 0 &&
        !process_find_area(address)) return -EFAULT;

    if (new_length == old_length) return (int64_t)address;

    struct vm_area *area = process_find_area(address);
    struct file *backing = area ? area->file : NULL;
    uint64_t backing_offset = area ? area->offset : 0;
    uint32_t kind = area ? area->kind : 0;
    uint64_t area_flags = area ? area->page_flags : PAGE_WRITE;

    if (backing) file_ref(backing);
    int64_t result = mremap_backed(process, address, old_length, new_length,
                                   flags, backing, backing_offset, kind, area_flags);
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

    process_protect_area(address, address + length, flags | PAGE_PRESENT);
    int reserved = !process_area_range_free(address, address + length);
    int failed = 0;
    vmm_flush_batch_begin();
    for (uint64_t page = address; page < address + length; page += 4096) {
        uint64_t old_flags;
        if (vmm_translate(process->cr3, page, NULL, &old_flags) != 0) {
            if (reserved) continue;
            failed = 1;
            break;
        }
        uint64_t effective_flags =
            flags | (old_flags & (PAGE_DEVICE | PAGE_SHARED | PAGE_FILEBACKED));
        if (nx_enabled && (old_flags & PAGE_DEVICE)) effective_flags |= PAGE_NX;

        int private_copy = (old_flags & PAGE_COW) ||
                           ((old_flags & PAGE_FILEBACKED) && !(old_flags & PAGE_SHARED));
        if (private_copy && (prot & PROT_WRITE))
            effective_flags = (effective_flags & ~PAGE_WRITE) | PAGE_COW;
        if (vmm_protect_page_in(process->cr3, page, effective_flags) != 0) {
            failed = 1;
            break;
        }
    }
    vmm_flush_batch_end();
    return failed ? -ENOMEM : 0;
}

static int vector_reserve(struct exec_vector *vector, size_t wanted) {
    if (wanted <= vector->capacity) return 0;
    size_t capacity = vector->capacity ? vector->capacity * 2 : 16;
    while (capacity < wanted) capacity *= 2;
    const char **items = (const char **)kmalloc(capacity * sizeof(*items));
    if (!items) return -ENOMEM;
    if (vector->count) memcpy(items, vector->items, vector->count * sizeof(*items));
    kfree(vector->items);
    vector->items = items;
    vector->capacity = capacity;
    return 0;
}

static void vector_free(struct exec_vector *vector) {
    for (size_t index = 0; index < vector->count; index++) kfree((void *)vector->items[index]);
    kfree(vector->items);
    vector->items = NULL;
    vector->count = vector->capacity = 0;
}

static void release_exec_arguments(struct exec_arguments **arguments) {
    if (!*arguments) return;
    vector_free(&(*arguments)->argv);
    vector_free(&(*arguments)->envp);
    kfree(*arguments);
}

static int vector_insert(struct exec_arguments *arguments, struct exec_vector *vector,
                         size_t at, const char *text) {
    size_t length = strlen(text);
    if (arguments->used + length + 1 + sizeof(char *) > arguments->budget) return -E2BIG;
    if (vector_reserve(vector, vector->count + 2) != 0) return -ENOMEM;
    char *copy = (char *)kmalloc(length + 1);
    if (!copy) return -ENOMEM;
    memcpy(copy, text, length + 1);
    for (size_t index = vector->count; index > at; index--)
        vector->items[index] = vector->items[index - 1];
    vector->items[at] = copy;
    vector->count++;
    vector->items[vector->count] = NULL;
    arguments->used += length + 1 + sizeof(char *);
    return 0;
}

static int copy_exec_vector(struct exec_arguments *arguments, uint64_t user_vector,
                            struct exec_vector *vector) {
    if (vector_reserve(vector, 1) != 0) return -ENOMEM;
    vector->items[0] = NULL;
    if (!user_vector) return 0;
    VFS_PATH_SCOPED scratch = (char *)kmalloc(MAX_ARG_STRLEN);
    if (!scratch) return -ENOMEM;
    for (uint64_t index = 0;; index++) {
        uint64_t user_string;
        if (copy_from_user(&user_string, user_vector + index * sizeof(uint64_t),
                           sizeof(user_string)) != 0) return -EFAULT;
        if (!user_string) return (int)vector->count;
        int copied = copy_string_from_user(scratch, MAX_ARG_STRLEN, user_string);
        if (copied == -2) return -E2BIG;
        if (copied < 0) return -EFAULT;
        int status = vector_insert(arguments, vector, vector->count, scratch);
        if (status != 0) return status;
        if (vector->count > 0x7FFFFFFFU) return -E2BIG;
    }
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

static int rewrite_script_arguments(struct exec_arguments *arguments,
                                    const char *script_path, const char *interpreter,
                                    const char *optional_argument) {
    struct exec_vector *argv = &arguments->argv;
    if (argv->count) {
        kfree((void *)argv->items[0]);
        for (size_t index = 1; index <= argv->count; index++)
            argv->items[index - 1] = argv->items[index];
        argv->count--;
    }
    int status = vector_insert(arguments, argv, 0, script_path);
    if (status == 0 && optional_argument && optional_argument[0])
        status = vector_insert(arguments, argv, 0, optional_argument);
    if (status == 0) status = vector_insert(arguments, argv, 0, interpreter);
    return status != 0 ? status : (int)argv->count;
}

static int64_t sys_execve(struct syscall_frame *frame, uint64_t user_path, uint64_t user_argv, uint64_t user_envp) {
    VFS_PATH_SCOPED path = NULL;
    VFS_PATH_SCOPED given = vfs_path_buffer();
    if (!given) return -ENOMEM;
    int status = copy_user_path(user_path, given);
    if (status != 0) return status;
    status = copy_path_at(AT_FDCWD, user_path, &path);
    if (status != 0) return status;
    __attribute__((cleanup(release_exec_arguments))) struct exec_arguments *arguments =
        (struct exec_arguments *)kmalloc(sizeof(*arguments));
    if (!arguments) return -ENOMEM;
    memset(arguments, 0, sizeof(*arguments));
    arguments->budget = process_arg_limit(process_current());
    int argc = copy_exec_vector(arguments, user_argv, &arguments->argv);
    if (argc < 0) return argc;
    int envc = copy_exec_vector(arguments, user_envp, &arguments->envp);
    if (envc < 0) return envc;
    if (argc == 0) {
        int status = vector_insert(arguments, &arguments->argv, 0, path);
        if (status != 0) return status;
        argc = 1;
    }
    if (envc == 0) {
        const char *defaults[] = {"PATH=/usr/bin:/usr/sbin:/bin:/sbin", "HOME=/", "TERM=tunix", "USER=root", NULL};
        for (int i = 0; defaults[i]; i++) {
            int status = vector_insert(arguments, &arguments->envp, (size_t)i, defaults[i]);
            if (status != 0) return status;
        }
    }

    struct vfs_node *file = vfs_lookup(path);
    if (!file) return -ENOENT;
    if ((file->flags & 0xFFU) != VFS_FILE) return -EACCES;
    if ((file->mode & 0111U) == 0) return -EACCES;
    int permitted = cred_may_path(path, file, CRED_EXEC);
    if (permitted != 0) return permitted;

    char interpreter[MAX_SHEBANG_LINE];
    char optional_argument[MAX_SHEBANG_LINE];
    int script = parse_shebang(file, interpreter, optional_argument);
    if (script < 0) return script;
    if (script > 0) {
        VFS_PATH_SCOPED interpreter_path = vfs_path_buffer();
        if (!interpreter_path || normalize_path(NULL, interpreter, interpreter_path) != 0)
            return -ENOENT;
        struct vfs_node *interpreter_file = vfs_lookup(interpreter_path);
        if (!interpreter_file || (interpreter_file->flags & 0xFFU) != VFS_FILE ||
            (interpreter_file->mode & 0111U) == 0) return -ENOENT;
        permitted = cred_may_path(interpreter_path, interpreter_file, CRED_EXEC);
        if (permitted != 0) return permitted;
        int rewritten = rewrite_script_arguments(arguments, given, interpreter,
                                                 optional_argument);
        if (rewritten < 0) return rewritten;
        int64_t result = process_exec_from_syscall(frame, interpreter_path,
                                                   arguments->argv.items, arguments->envp.items,
                                                   NULL);
        return result == -1 ? -ENOEXEC : result;
    }

    int64_t result = process_exec_from_syscall(frame, path, arguments->argv.items,
                                               arguments->envp.items, file);
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

static void watch_blocked_file(struct process *process, uint64_t syscall_number,
                               struct file *file) {
    uint32_t events;
    if (syscall_number == SYS_READ || syscall_number == SYS_READV ||
        syscall_number == SYS_RECVFROM || syscall_number == SYS_RECVMSG ||
        syscall_number == SYS_RECVMMSG || syscall_number == SYS_ACCEPT ||
        syscall_number == SYS_ACCEPT4) events = POLLIN;
    else if (syscall_number == SYS_WRITE || syscall_number == SYS_WRITEV ||
             syscall_number == SYS_SENDTO || syscall_number == SYS_SENDMSG ||
             syscall_number == SYS_CONNECT) events = POLLOUT;
    else return;
    if (!process || !process->files || !file) return;
    int fd = file_table_find(process->files, file);
    if (fd < 0) return;
    io_watch_begin(process);
    io_watch_add(process, fd, events);
}

static void block_and_retry(struct syscall_frame *frame, uint64_t syscall_number,
                            struct file *file, int writing) {
    if (process_signal_interrupts_wait()) {
        SYSCALL_RET(frame) = (uint64_t)-(int64_t)EINTR;
        return;
    }
    SYSCALL_RESTART(frame, syscall_number);
    struct process *process = process_current();
    if (process) process->syscall_rewound = 1;

    const void *channel = writing ? file_write_wait_channel(file)
                                  : file_read_wait_channel(file);
    if (!channel) {
        channel = process_io_wait_channel();
        watch_blocked_file(process, syscall_number, file);
    }
    if (process_sleep_on(frame, channel) != 0)
        process_yield_from_syscall(frame);
}

static int64_t sys_readv_writev(int fd, uint64_t user_iov, int count, int write_mode) {
    if (count < 0 || count > 1024) return -EINVAL;
    struct file *file = file_from_fd(fd);
    if (write_mode && file && file->kind == FILE_KIND_INET_SOCKET && count <= 16) {
        struct linux_msghdr message;
        memset(&message, 0, sizeof(message));
        message.iov = user_iov;
        message.iov_length = (uint64_t)count;
        uint8_t data[4096];
        size_t length = 0;
        int status = copy_message_iovecs(&message, data, sizeof(data), &length, 1, 0);
        if (status != -EMSGSIZE) {
            if (status < 0) return status;
            int64_t result = file_write(file, length, data);
            if (result == -EPIPE)
                (void)process_send_signal((int64_t)process_current()->pid, SIGPIPE);
            return result;
        }
    }
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

    value.totalram = pmm_usable_page_count() * PMM_PAGE_SIZE;
    value.freeram = pmm_free_page_count() * PMM_PAGE_SIZE;
    value.procs = (uint16_t)process_count();
    value.mem_unit = 1;
    return copy_to_user(user_buffer, &value, sizeof(value)) == 0 ? 0 : -EFAULT;
}

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
    if (process) {
        memcpy(value + 128, &process->voluntary_switches, sizeof(uint64_t));
        memcpy(value + 136, &process->involuntary_switches, sizeof(uint64_t));
    }
    return copy_to_user(user_buffer, value, sizeof(value)) == 0 ? 0 : -EFAULT;
}

#define SYSLOG_ACTION_CLOSE 0
#define SYSLOG_ACTION_OPEN 1
#define SYSLOG_ACTION_READ 2
#define SYSLOG_ACTION_READ_ALL 3
#define SYSLOG_ACTION_READ_CLEAR 4
#define SYSLOG_ACTION_CLEAR 5
#define SYSLOG_ACTION_CONSOLE_OFF 6
#define SYSLOG_ACTION_CONSOLE_ON 7
#define SYSLOG_ACTION_CONSOLE_LEVEL 8
#define SYSLOG_ACTION_SIZE_UNREAD 9
#define SYSLOG_ACTION_SIZE_BUFFER 10

static int64_t sys_syslog(int action, uint64_t user_buffer, int length) {
    switch (action) {
    case SYSLOG_ACTION_CLOSE:
    case SYSLOG_ACTION_OPEN:
    case SYSLOG_ACTION_CLEAR:
        return 0;
    case SYSLOG_ACTION_CONSOLE_OFF: klog_console(0); return 0;
    case SYSLOG_ACTION_CONSOLE_ON: klog_console(1); return 0;
    case SYSLOG_ACTION_CONSOLE_LEVEL: return 0;
    case SYSLOG_ACTION_SIZE_UNREAD: return 0;
    case SYSLOG_ACTION_SIZE_BUFFER: return (int64_t)klog_size();
    case SYSLOG_ACTION_READ:
    case SYSLOG_ACTION_READ_ALL:
    case SYSLOG_ACTION_READ_CLEAR: {
        if (length < 0 || !user_buffer) return -EINVAL;
        size_t held = klog_size();
        size_t want = (size_t)length;

        uint64_t offset = held > want ? held - want : 0;
        if (want > held - offset) want = held - offset;
        char staging[512];
        size_t produced = 0;
        while (produced < want) {
            size_t chunk = want - produced;
            if (chunk > sizeof(staging)) chunk = sizeof(staging);
            int64_t got = klog_read(offset + produced, chunk, staging);
            if (got <= 0) break;
            if (copy_to_user(user_buffer + produced, staging, (size_t)got) != 0)
                return produced ? (int64_t)produced : -EFAULT;
            produced += (size_t)got;
        }
        return (int64_t)produced;
    }
    default: return -EINVAL;
    }
}

#define MODULE_IMAGE_MAX (16ULL * 1024ULL * 1024ULL)
#define MODULE_ARGUMENTS_MAX 256

static int64_t module_arguments(char *out, size_t capacity, uint64_t user_arguments) {
    out[0] = '\0';
    if (!user_arguments) return 0;
    return copy_string_from_user(out, capacity, user_arguments) < 0 ? -EFAULT : 0;
}

static int64_t sys_init_module(uint64_t user_image, uint64_t length,
                               uint64_t user_arguments) {
    if (!cred_is_root()) return -EPERM;
    if (!length || length > MODULE_IMAGE_MAX) return -EINVAL;
    char arguments[MODULE_ARGUMENTS_MAX];
    int64_t status = module_arguments(arguments, sizeof(arguments), user_arguments);
    if (status != 0) return status;

    void *image = kmalloc((size_t)length);
    if (!image) return -ENOMEM;
    if (copy_from_user(image, user_image, (size_t)length) != 0) {
        kfree(image);
        return -EFAULT;
    }
    int64_t result = module_load(image, (size_t)length, arguments);
    kfree(image);
    return result;
}

static int64_t sys_finit_module(int fd, uint64_t user_arguments, int flags) {
    (void)flags;
    if (!cred_is_root()) return -EPERM;
    struct file *file = file_from_fd(fd);
    if (!file || file->kind != FILE_KIND_VFS || !file->node) return -EBADF;
    uint64_t length = file->node->length;
    if (!length || length > MODULE_IMAGE_MAX) return -EINVAL;

    char arguments[MODULE_ARGUMENTS_MAX];
    int64_t status = module_arguments(arguments, sizeof(arguments), user_arguments);
    if (status != 0) return status;

    void *image = kmalloc((size_t)length);
    if (!image) return -ENOMEM;
    int64_t got = vfs_read(file->node, 0, (size_t)length, image);
    int64_t result = got == (int64_t)length
        ? module_load(image, (size_t)length, arguments) : -EIO;
    kfree(image);
    return result;
}

static int64_t sys_delete_module(uint64_t user_name, uint32_t flags) {
    if (!cred_is_root()) return -EPERM;
    char name[MODULE_NAME_MAX];
    if (copy_string_from_user(name, sizeof(name), user_name) < 0) return -EFAULT;
    return module_unload(name, flags);
}

static int64_t sys_uname(uint64_t user_buffer) {
    struct linux_utsname value;
    memset(&value, 0, sizeof(value));
    strncpy(value.sysname, UTS_SYSNAME, sizeof(value.sysname) - 1);
    strncpy(value.nodename, uts_hostname(), sizeof(value.nodename) - 1);
    strncpy(value.release, UTS_RELEASE, sizeof(value.release) - 1);
    strncpy(value.version, UTS_VERSION, sizeof(value.version) - 1);
    strncpy(value.machine, SYSCALL_UTS_MACHINE, sizeof(value.machine) - 1);
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

static int64_t sys_sched_getaffinity(uint64_t tid, size_t size, uint64_t user_mask) {
    if (!user_mask) return -EFAULT;
    if (size < sizeof(uint64_t) || (size & (sizeof(uint64_t) - 1))) return -EINVAL;

    struct cpu_mask mask;
    int result = process_get_affinity(tid, &mask);
    if (result != 0) return result;

    size_t bytes = size > sizeof(mask) ? sizeof(mask) : size;
    if (copy_to_user(user_mask, &mask, bytes) != 0) return -EFAULT;
    for (size_t offset = bytes; offset < size; offset += sizeof(uint64_t)) {
        uint64_t zero = 0;
        if (copy_to_user(user_mask + offset, &zero, sizeof(zero)) != 0) return -EFAULT;
    }
    return (int64_t)size;
}

static int64_t sys_sched_setaffinity(uint64_t tid, size_t size, uint64_t user_mask) {
    if (!user_mask) return -EFAULT;
    if (size < sizeof(uint64_t)) return -EINVAL;
    struct cpu_mask mask;
    memset(&mask, 0, sizeof(mask));
    size_t bytes = size > sizeof(mask) ? sizeof(mask) : size;
    if (copy_from_user(&mask, user_mask, bytes) != 0) return -EFAULT;
    return process_set_affinity(tid, &mask);
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
    if (code == ARCH_SET_GS) {
        if (address >= USER_ADDRESS_LIMIT) return -EINVAL;
        process_set_gs_base(address);
        return 0;
    }
    if (code == ARCH_GET_GS) {
        uint64_t value = process_get_gs_base();
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
    int on_stack = signal_stack_active(process, SYSCALL_USER_SP(frame));

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

    uint32_t target_euid = 0;
    if (header.pid) {
        process_table_lock();
        struct process *target = process_find((uint64_t)header.pid);
        if (target) target_euid = target->cred.euid;
        process_table_unlock();
        if (!target) return -ESRCH;
    } else {
        target_euid = process_current()->cred.euid;
    }

    uint32_t bits = target_euid == 0 ? 0xFFFFFFFFU : 0U;
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
    if (cred_is_root()) return 0;

    struct cap_user_data data[2];
    if (copy_from_user(data, user_data, sizeof(data)) != 0) return -EFAULT;
    if (data[0].effective | data[0].permitted | data[0].inheritable |
        data[1].effective | data[1].permitted | data[1].inheritable)
        return -EPERM;
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
            return process->keep_capabilities;
        case PR_SET_KEEPCAPS:
            if (arg2 > 1) return -EINVAL;

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
    uint64_t head = 0;
    uint64_t length = 0;
    process_table_lock();
    struct process *target = pid == 0 ? process_current() : process_find((uint64_t)pid);
    if (target) {
        head = target->robust_list_head;
        length = target->robust_list_length;
    }
    process_table_unlock();
    if (!target) return -ESRCH;
    if (copy_to_user(user_head_pointer, &head, sizeof(head)) != 0) return -EFAULT;
    return copy_to_user(user_length_pointer, &length, sizeof(length)) == 0 ? 0 : -EFAULT;
}

static int64_t sys_prlimit(uint64_t pid, uint64_t resource, uint64_t user_new_limit,
                           uint64_t user_old_limit) {
    if (resource >= PROCESS_RLIMITS) return -EINVAL;
    struct linux_rlimit wanted;
    if (user_new_limit && copy_from_user(&wanted, user_new_limit, sizeof(wanted)) != 0)
        return -EFAULT;
    const struct credentials *cred = cred_current();
    int privileged = !cred || cred->euid == 0;
    struct process_rlimit previous = {0, 0};
    int64_t status = 0;
    process_table_lock();
    struct process *target = pid ? process_find(pid) : process_current();
    if (!target) status = -ESRCH;
    else if (target != process_current() && !privileged && cred->euid != target->cred.uid)
        status = -EPERM;
    else previous = target->rlimits[resource];
    if (status == 0 && user_new_limit) {
        struct process_rlimit value = {wanted.rlim_cur, wanted.rlim_max};
        if (wanted.rlim_cur > wanted.rlim_max) status = -EINVAL;
        else if (wanted.rlim_max > previous.hard && !privileged) status = -EPERM;
        else if (process_set_rlimit(target, (unsigned)resource, &value) != 0) status = -EPERM;
    }
    process_table_unlock();
    if (status != 0) return status;
    if (user_old_limit) {
        struct linux_rlimit old = {previous.soft, previous.hard};
        if (copy_to_user(user_old_limit, &old, sizeof(old)) != 0) return -EFAULT;
    }
    return 0;
}

static int64_t sys_clone_fork_compat(struct syscall_frame *frame,
                                     uint64_t flags, uint64_t child_stack,
                                     uint64_t parent_tid_user, uint64_t child_tid_user,
                                     uint64_t tls, int clear_handlers) {
    struct process *parent = process_current();
    if (!parent) return -EINVAL;
    if ((flags & CLONE_PARENT_SETTID) &&
        (!parent_tid_user || !vmm_user_range_valid(parent->cr3, parent_tid_user,
                                                    sizeof(uint32_t), 1)))
        return -EFAULT;
    if ((flags & CLONE_CHILD_SETTID) &&
        (!child_tid_user || !vmm_user_range_valid(parent->cr3, child_tid_user,
                                                   sizeof(uint32_t), 1)))
        return -EFAULT;

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

    if ((flags & CLONE_VFORK) && (flags & CLONE_VM) && child_stack) {
        uint64_t vfork_allowed = CLONE_VM | CLONE_VFORK | CLONE_FS | CLONE_FILES |
                                 CLONE_SIGHAND | CLONE_SYSVSEM | CLONE_SETTLS |
                                 CLONE_FORK_METADATA_FLAGS;
        if (flags & ~(0xFFULL | vfork_allowed)) return -ENOSYS;
        if (exit_signal != 0 && exit_signal != SIGCHLD) return -EINVAL;
        struct fork_request request = {child_stack, 0, 0, clear_handlers};
        return process_fork_from_syscall(frame, &request);
    }

    uint64_t unsupported = flags & ~(0xFFULL | CLONE_FORK_METADATA_FLAGS);
    if (unsupported != 0 || (flags & CLONE_FORK_REJECT_FLAGS) != 0) {
        KDEBUG("syscall: clone unsupported flags=0x%llx stack=0x%llx\n",
                (unsigned long long)flags, (unsigned long long)child_stack);
        return -ENOSYS;
    }
    if (exit_signal != 0 && exit_signal != SIGCHLD) return -EINVAL;

    struct fork_request request = {
        child_stack,
        (flags & CLONE_CHILD_SETTID) ? child_tid_user : 0,
        (flags & CLONE_CHILD_CLEARTID) ? child_tid_user : 0,
        clear_handlers,
    };
    int64_t pid = process_fork_from_syscall(frame, &request);
    if (pid <= 0) return pid;

    uint32_t tid = (uint32_t)pid;
    if ((flags & CLONE_PARENT_SETTID) && parent_tid_user) {
        if (copy_to_user(parent_tid_user, &tid, sizeof(tid)) != 0) return -EFAULT;
    }
    return pid;
}

static int64_t sys_clone3_fork_compat(struct syscall_frame *frame,
                                      uint64_t user_args, size_t size) {
    struct linux_clone_args args;
    if (size < 64) return -EINVAL;
    if (size > sizeof(args)) return -E2BIG;
    memset(&args, 0, sizeof(args));
    if (copy_from_user(&args, user_args, size) != 0) return -EFAULT;
    if ((args.flags & 0xFFULL) || args.exit_signal > TUNIX_NSIG ||
        (args.flags & CLONE_DETACHED) ||
        ((args.flags & CLONE_CLEAR_SIGHAND) && (args.flags & CLONE_SIGHAND)))
        return -EINVAL;
    if ((!args.stack && args.stack_size) || (args.stack && !args.stack_size) ||
        args.stack >= USER_ADDRESS_LIMIT ||
        args.stack_size > USER_ADDRESS_LIMIT - args.stack)
        return -EINVAL;
    if ((args.flags & CLONE_PIDFD) || args.set_tid_size ||
        (args.flags & CLONE_INTO_CGROUP))
        return -ENOSYS;

    uint64_t child_stack = args.stack ? args.stack + args.stack_size : 0;
    uint64_t flags = (args.flags & ~CLONE_CLEAR_SIGHAND) | args.exit_signal;
    return sys_clone_fork_compat(frame, flags, child_stack, args.parent_tid,
                                 args.child_tid, args.tls,
                                 (args.flags & CLONE_CLEAR_SIGHAND) != 0);
}

static struct file *file_from_fd(int fd) {
    return fd_file(fd);
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

static int64_t sys_memfd_create(uint64_t user_name, uint32_t flags) {
    if (flags & ~(uint32_t)(MFD_CLOEXEC | MFD_ALLOW_SEALING | MFD_NOEXEC_SEAL |
                            MFD_EXEC)) return -EINVAL;
    char name[250];
    int copied = copy_string_from_user(name, sizeof(name), user_name);
    if (copied == -2) return -EINVAL;
    if (copied < 0) return -EFAULT;

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

static int64_t sys_signalfd(int fd, uint64_t user_mask, uint64_t mask_size,
                            int flags) {
    struct process *process = process_current();
    if (!process) return -EINVAL;
    if (flags & ~(SFD_NONBLOCK | SFD_CLOEXEC)) return -EINVAL;
    if (mask_size != sizeof(uint64_t)) return -EINVAL;

    uint64_t mask = 0;
    if (copy_from_user(&mask, user_mask, sizeof(mask)) != 0) return -EFAULT;

    if (fd >= 0) {
        if (fd >= PROCESS_FD_CAPACITY(process) || !fd_file(fd)) return -EBADF;
        struct file *file = fd_file(fd);
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

static void release_events(struct tunix_epoll_event **events) {
    kfree(*events);
}

static int64_t sys_epoll_wait_once(int epoll_fd, uint64_t user_events,
                                   int maximum, int commit_empty) {
    if (!user_events || maximum <= 0) return -EINVAL;

    struct file *file = file_from_fd(epoll_fd);
    if (!file || file->kind != FILE_KIND_EPOLL) return -EBADF;
    int entries = epoll_entry_count(file->epoll);
    if (maximum > entries) maximum = entries > 0 ? entries : 1;
    __attribute__((cleanup(release_events))) struct tunix_epoll_event *events =
        (struct tunix_epoll_event *)kmalloc((size_t)maximum * sizeof(*events));
    if (!events) return -ENOMEM;
    int ready = epoll_collect(file->epoll, events, maximum, 0);
    if (ready < 0) return ready;
    if (!ready) {
        io_watch_begin(process_current());
        io_watch_add(process_current(), epoll_fd, POLLIN);
    }
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
    VFS_PATH_SCOPED path = NULL;
    int status = copy_path_at(AT_FDCWD, user_path, &path);
    if (status != 0) return status;
    struct vfs_node *node = vfs_lookup(path);
    if (!node) return -ENOENT;
    return inotify_add_watch(file->inotify, node, mask);
}

static int64_t sys_inotify_rm_watch(int fd, int descriptor) {
    struct file *file = file_from_fd(fd);
    if (!file || file->kind != FILE_KIND_INOTIFY) return -EBADF;
    return inotify_remove_watch(file->inotify, descriptor);
}

static uint64_t file_cache_budget(void) {
    static uint64_t budget;
    if (budget) return budget;
    budget = pmm_usable_page_count() * PMM_PAGE_SIZE / 16ULL;
    if (budget < 32ULL * 1024 * 1024) budget = 32ULL * 1024 * 1024;
    if (budget > 128ULL * 1024 * 1024) budget = 128ULL * 1024 * 1024;
    return budget;
}

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
        case LINUX_REBOOT_CMD_CAD_ON:  power_set_button_handled(1); return 0;
        case LINUX_REBOOT_CMD_CAD_OFF: power_set_button_handled(0); return 0;
        case LINUX_REBOOT_CMD_RESTART:   power_restart();
        case LINUX_REBOOT_CMD_HALT:      power_halt();
        case LINUX_REBOOT_CMD_POWER_OFF: power_off();
        default: return -EINVAL;
    }
}

static void note_would_block(struct syscall_frame *frame, uint64_t number, uint64_t fd) {
    if ((int64_t)SYSCALL_RET(frame) != -(int64_t)EAGAIN) return;
    if (number != SYS_READ && number != SYS_WRITE && number != SYS_READV &&
        number != SYS_WRITEV && number != SYS_RECVFROM && number != SYS_SENDTO &&
        number != SYS_RECVMSG && number != SYS_SENDMSG && number != SYS_RECVMMSG &&
        number != SYS_SENDMMSG && number != SYS_ACCEPT && number != SYS_ACCEPT4) return;
    struct file *file = file_from_fd((int)fd);
    if (file) file->edge_generation++;
}


static int positional_file(struct file *file) {
    return file && (file->kind == FILE_KIND_VFS || file->kind == FILE_KIND_MEMFD);
}

static int64_t sys_pread_pwrite(int fd, uint64_t user_buffer, size_t length, uint64_t offset,
                                int writing) {
    struct file *file = fd_file(fd);
    if (!positional_file(file)) return -EBADF;
    uint8_t buffer[4096];
    size_t completed = 0;
    while (completed < length) {
        size_t chunk = length - completed;
        if (chunk > sizeof(buffer)) chunk = sizeof(buffer);
        int64_t amount;
        if (writing) {
            if (copy_from_user(buffer, user_buffer + completed, chunk) != 0)
                return completed ? (int64_t)completed : -EFAULT;
            amount = file_pwrite(file, offset + completed, chunk, buffer);
        } else {
            amount = file_pread(file, offset + completed, chunk, buffer);
            if (amount > 0 && copy_to_user(user_buffer + completed, buffer, (size_t)amount) != 0)
                return completed ? (int64_t)completed : -EFAULT;
        }
        if (amount < 0) return completed ? (int64_t)completed : amount;
        if (amount == 0) break;
        completed += (size_t)amount;
        if ((size_t)amount < chunk) break;
    }
    return (int64_t)completed;
}

static int64_t sys_preadv_pwritev(int fd, uint64_t user_iov, int count, uint64_t offset,
                                  int writing) {
    if (count < 0 || count > 1024) return -EINVAL;
    if (!positional_file(fd_file(fd))) return -EBADF;
    int64_t total = 0;
    for (int index = 0; index < count; index++) {
        struct linux_iovec iov;
        if (copy_from_user(&iov, user_iov + (uint64_t)index * sizeof(iov), sizeof(iov)) != 0)
            return total ? total : -EFAULT;
        int64_t result = sys_pread_pwrite(fd, iov.base, (size_t)iov.length,
                                          offset + (uint64_t)total, writing);
        if (result < 0) return total ? total : result;
        total += result;
        if ((uint64_t)result < iov.length) break;
    }
    return total;
}

#define MEMORY_SYSCALL(name, params, args) \
    static int64_t memory_call_##name params { \
        process_memory_enter(); \
        int64_t result = sys_##name args; \
        process_memory_leave(); \
        return result; \
    }

MEMORY_SYSCALL(mmap, (uint64_t a, uint64_t b, int c, int d, int e, uint64_t f), (a, b, c, d, e, f))
MEMORY_SYSCALL(mprotect, (uint64_t a, uint64_t b, int c), (a, b, c))
MEMORY_SYSCALL(mremap, (uint64_t a, uint64_t b, uint64_t c, int d, uint64_t e), (a, b, c, d, e))
MEMORY_SYSCALL(msync, (uint64_t a, uint64_t b, int c), (a, b, c))
MEMORY_SYSCALL(munmap, (uint64_t a, uint64_t b), (a, b))
MEMORY_SYSCALL(shmat, (int a, uint64_t b, int c), (a, b, c))
MEMORY_SYSCALL(shmdt, (uint64_t a), (a))
MEMORY_SYSCALL(brk, (uint64_t a), (a))

static void syscall_run(struct syscall_frame *frame) {
    if (!frame) return;
    process_reap_deferred();

    vfs_trim_cache(file_cache_budget());
    if (heap_under_pressure()) vfs_reclaim_file_data(vfs_root);
    struct process *caller = process_current();
    uint64_t syscall_number = SYSCALL_NR(frame);
    uint64_t first_argument = SYSCALL_ARG0(frame);
    if (caller) {
        caller->syscall_rewound = 0;
        caller->io_watch_armed = 0;
    }
    if (caller && caller->io_wait_active && caller->io_wait_syscall != syscall_number)
        clear_io_wait(caller);
    int skip_signal_delivery = 0;

    switch (syscall_number) {
        case SYS_READ: {
            int fd = (int)SYSCALL_ARG0(frame);
            int64_t result = sys_read(fd, SYSCALL_ARG1(frame), (size_t)SYSCALL_ARG2(frame));
            struct process *process = process_current();
            struct file *file = process && fd >= 0 && fd < PROCESS_FD_CAPACITY(process) ? fd_file(fd) : NULL;
            if (result == -EAGAIN && file && !(file->flags & O_NONBLOCK)) {
                block_and_retry(frame, SYS_READ, file, 0);
            } else {
                SYSCALL_RET(frame) = (uint64_t)result;
            }
            break;
        }
        case SYS_WRITE: {
            int fd = (int)SYSCALL_ARG0(frame);
            int64_t result = sys_write(fd, SYSCALL_ARG1(frame), (size_t)SYSCALL_ARG2(frame));
            struct file *file = file_from_fd(fd);
            if (result == -EAGAIN && file && !(file->flags & O_NONBLOCK)) {
                block_and_retry(frame, SYS_WRITE, file, 1);
            } else {
                SYSCALL_RET(frame) = (uint64_t)result;
            }
            break;
        }
        case SYS_OPEN: SYSCALL_RET(frame) = (uint64_t)open_at(AT_FDCWD, SYSCALL_ARG0(frame), SYSCALL_OPEN_FLAGS_IN(SYSCALL_ARG1(frame)), SYSCALL_ARG2(frame)); break;
        case SYS_CLOSE: SYSCALL_RET(frame) = (uint64_t)sys_close((int)SYSCALL_ARG0(frame)); break;
        case SYS_POLL: {
            int timeout_ms = (int)SYSCALL_ARG2(frame);
            int64_t timeout_ns = timeout_ms_to_ns(timeout_ms);
            int64_t result = sys_poll_once(SYSCALL_ARG0(frame), SYSCALL_ARG1(frame), timeout_ms == 0);
            if (result != 0 || timeout_ms == 0) {
                clear_io_wait(process_current());
                SYSCALL_RET(frame) = (uint64_t)result;
            } else if (!retry_io_wait(frame, SYS_POLL, timeout_ns)) {
                SYSCALL_RET(frame) = (uint64_t)sys_poll_once(SYSCALL_ARG0(frame), SYSCALL_ARG1(frame), 1);
            }
            break;
        }
        case SYS_STAT: SYSCALL_RET(frame) = (uint64_t)stat_path(AT_FDCWD, SYSCALL_ARG0(frame), SYSCALL_ARG1(frame), 1); break;
        case SYS_LSTAT: SYSCALL_RET(frame) = (uint64_t)stat_path(AT_FDCWD, SYSCALL_ARG0(frame), SYSCALL_ARG1(frame), 0); break;
        case SYS_FSTAT: SYSCALL_RET(frame) = (uint64_t)sys_fstat((int)SYSCALL_ARG0(frame), SYSCALL_ARG1(frame)); break;
        case SYS_LSEEK: SYSCALL_RET(frame) = (uint64_t)sys_lseek((int)SYSCALL_ARG0(frame), (int64_t)SYSCALL_ARG1(frame), (int)SYSCALL_ARG2(frame)); break;
        case SYS_MMAP: SYSCALL_RET(frame) = (uint64_t)memory_call_mmap(SYSCALL_ARG0(frame), SYSCALL_ARG1(frame), (int)SYSCALL_ARG2(frame), (int)SYSCALL_ARG3(frame), (int)SYSCALL_ARG4(frame), SYSCALL_ARG5(frame)); break;
        case SYS_MPROTECT: SYSCALL_RET(frame) = (uint64_t)memory_call_mprotect(SYSCALL_ARG0(frame), SYSCALL_ARG1(frame), (int)SYSCALL_ARG2(frame)); break;
        case SYS_MREMAP:
            SYSCALL_RET(frame) = (uint64_t)memory_call_mremap(SYSCALL_ARG0(frame), SYSCALL_ARG1(frame), SYSCALL_ARG2(frame),
                                              (int)SYSCALL_ARG3(frame), SYSCALL_ARG4(frame));
            break;

        case SYS_MADVISE: SYSCALL_RET(frame) = 0; break;
        case SYS_FADVISE64: SYSCALL_RET(frame) = 0; break;
        case SYS_MSYNC:
            SYSCALL_RET(frame) = (uint64_t)memory_call_msync(SYSCALL_ARG0(frame), SYSCALL_ARG1(frame), (int)SYSCALL_ARG2(frame));
            break;
        case SYS_MUNMAP: SYSCALL_RET(frame) = (uint64_t)memory_call_munmap(SYSCALL_ARG0(frame), SYSCALL_ARG1(frame)); break;
        case SYS_SHMGET:
            SYSCALL_RET(frame) = (uint64_t)sys_shmget((int32_t)SYSCALL_ARG0(frame), SYSCALL_ARG1(frame), (int)SYSCALL_ARG2(frame));
            break;
        case SYS_SHMAT:
            SYSCALL_RET(frame) = (uint64_t)memory_call_shmat((int)SYSCALL_ARG0(frame), SYSCALL_ARG1(frame), (int)SYSCALL_ARG2(frame));
            break;
        case SYS_SHMDT: SYSCALL_RET(frame) = (uint64_t)memory_call_shmdt(SYSCALL_ARG0(frame)); break;
        case SYS_SHMCTL:
            SYSCALL_RET(frame) = (uint64_t)sys_shmctl((int)SYSCALL_ARG0(frame), (int)SYSCALL_ARG1(frame), SYSCALL_ARG2(frame));
            break;
        case SYS_BRK: SYSCALL_RET(frame) = (uint64_t)memory_call_brk(SYSCALL_ARG0(frame)); break;
        case SYS_RT_SIGACTION: SYSCALL_RET(frame) = (uint64_t)sys_sigaction((int)SYSCALL_ARG0(frame), SYSCALL_ARG1(frame), SYSCALL_ARG2(frame), SYSCALL_ARG3(frame)); break;
        case SYS_RT_SIGPROCMASK: SYSCALL_RET(frame) = (uint64_t)sys_sigprocmask((int)SYSCALL_ARG0(frame), SYSCALL_ARG1(frame), SYSCALL_ARG2(frame), SYSCALL_ARG3(frame)); break;
        case SYS_RT_SIGRETURN:
            if (process_sigreturn(frame) != 0) SYSCALL_RET(frame) = (uint64_t)-(int64_t)EINVAL;
            skip_signal_delivery = 1;
            break;
        case SYS_IOCTL: {
            int fd = (int)SYSCALL_ARG0(frame);
            int64_t result = sys_ioctl(fd, (unsigned long)SYSCALL_ARG1(frame), SYSCALL_ARG2(frame));
            struct file *file = file_from_fd(fd);

            if (result == -EAGAIN && file && !(file->flags & O_NONBLOCK) &&
                file->kind == FILE_KIND_VFS && file->node &&
                file->node->write_ready) {
                block_and_retry(frame, SYS_IOCTL, file, 1);
            } else if (result == -EAGAIN && (unsigned long)SYSCALL_ARG1(frame) == VT_WAITACTIVE) {
                if (process_signal_interrupts_wait()) {
                    SYSCALL_RET(frame) = (uint64_t)-(int64_t)EINTR;
                    break;
                }
                SYSCALL_RESTART(frame, SYS_IOCTL);
                struct process *waiter = process_current();
                if (waiter) waiter->syscall_rewound = 1;
                if (process_sleep_on(frame, vt_switch_wait_channel()) != 0)
                    process_yield_from_syscall(frame);
            } else {
                SYSCALL_RET(frame) = (uint64_t)result;
            }
            break;
        }
        case SYS_PREAD64:
            SYSCALL_RET(frame) = (uint64_t)sys_pread_pwrite((int)SYSCALL_ARG0(frame), SYSCALL_ARG1(frame),
                                                         (size_t)SYSCALL_ARG2(frame), SYSCALL_ARG3(frame), 0);
            break;
        case SYS_PWRITE64:
            SYSCALL_RET(frame) = (uint64_t)sys_pread_pwrite((int)SYSCALL_ARG0(frame), SYSCALL_ARG1(frame),
                                                         (size_t)SYSCALL_ARG2(frame), SYSCALL_ARG3(frame), 1);
            break;
        case SYS_PREADV:
        case SYS_PWRITEV:
            SYSCALL_RET(frame) = (uint64_t)sys_preadv_pwritev((int)SYSCALL_ARG0(frame), SYSCALL_ARG1(frame),
                                                           (int)SYSCALL_ARG2(frame), SYSCALL_ARG3(frame),
                                                           syscall_number == SYS_PWRITEV);
            break;
        case SYS_READV:
        case SYS_WRITEV: {
            int writing = syscall_number == SYS_WRITEV;
            int fd = (int)SYSCALL_ARG0(frame);
            int64_t result = sys_readv_writev(fd, SYSCALL_ARG1(frame), (int)SYSCALL_ARG2(frame), writing);

            struct file *file = file_from_fd(fd);
            if (result == -EAGAIN && file && !(file->flags & O_NONBLOCK))
                block_and_retry(frame, syscall_number, file, writing);
            else
                SYSCALL_RET(frame) = (uint64_t)result;
            break;
        }
        case SYS_ACCESS:
            SYSCALL_RET(frame) = (uint64_t)sys_faccess_at(AT_FDCWD, SYSCALL_ARG0(frame), (int)SYSCALL_ARG1(frame), 0);
            break;
        case SYS_PSELECT6: {
            int64_t timeout_ns = read_timespec_timeout_ns(SYSCALL_ARG4(frame));
            if (timeout_ns < -1) {
                clear_io_wait(process_current());
                process_restore_signal_mask();
                SYSCALL_RET(frame) = (uint64_t)timeout_ns;
                break;
            }
            apply_wait_signal_mask(SYSCALL_ARG5(frame));
            int64_t result = sys_select_once((int)SYSCALL_ARG0(frame), SYSCALL_ARG1(frame), SYSCALL_ARG2(frame),
                                             SYSCALL_ARG3(frame), timeout_ns == 0);
            if (result != 0 || timeout_ns == 0) {
                clear_io_wait(process_current());
                process_restore_signal_mask();
                SYSCALL_RET(frame) = (uint64_t)result;
            } else if (!retry_io_wait(frame, SYS_PSELECT6, timeout_ns)) {
                process_restore_signal_mask();
                SYSCALL_RET(frame) = (uint64_t)sys_select_once((int)SYSCALL_ARG0(frame), SYSCALL_ARG1(frame),
                                                       SYSCALL_ARG2(frame), SYSCALL_ARG3(frame), 1);
            } else if (!process_current()->syscall_rewound) {
                process_restore_signal_mask();
            }
            break;
        }
        case SYS_PPOLL: {
            int64_t timeout_ns = read_timespec_timeout_ns(SYSCALL_ARG2(frame));
            if (timeout_ns < -1) {
                clear_io_wait(process_current());
                process_restore_signal_mask();
                SYSCALL_RET(frame) = (uint64_t)timeout_ns;
                break;
            }
            apply_wait_signal_set(SYSCALL_ARG3(frame), SYSCALL_ARG4(frame));
            int64_t result = sys_poll_once(SYSCALL_ARG0(frame), SYSCALL_ARG1(frame), timeout_ns == 0);
            if (result != 0 || timeout_ns == 0) {
                clear_io_wait(process_current());
                process_restore_signal_mask();
                SYSCALL_RET(frame) = (uint64_t)result;
            } else if (!retry_io_wait(frame, SYS_PPOLL, timeout_ns)) {
                process_restore_signal_mask();
                SYSCALL_RET(frame) = (uint64_t)sys_poll_once(SYSCALL_ARG0(frame), SYSCALL_ARG1(frame), 1);
            } else if (!process_current()->syscall_rewound) {
                process_restore_signal_mask();
            }
            break;
        }
        case SYS_FACCESSAT:
            SYSCALL_RET(frame) = (uint64_t)sys_faccess_at((int)SYSCALL_ARG0(frame), SYSCALL_ARG1(frame),
                                                  (int)SYSCALL_ARG2(frame), 0);
            break;
        case SYS_PIPE: SYSCALL_RET(frame) = (uint64_t)sys_pipe(SYSCALL_ARG0(frame), 0); break;
        case SYS_SELECT: {
            int64_t timeout_ns = read_timeval_timeout_ns(SYSCALL_ARG4(frame));
            if (timeout_ns < -1) {
                clear_io_wait(process_current());
                SYSCALL_RET(frame) = (uint64_t)timeout_ns;
                break;
            }
            int64_t result = sys_select_once((int)SYSCALL_ARG0(frame), SYSCALL_ARG1(frame), SYSCALL_ARG2(frame),
                                             SYSCALL_ARG3(frame), timeout_ns == 0);
            if (result != 0 || timeout_ns == 0) {
                clear_io_wait(process_current());
                SYSCALL_RET(frame) = (uint64_t)result;
            } else if (!retry_io_wait(frame, SYS_SELECT, timeout_ns)) {
                SYSCALL_RET(frame) = (uint64_t)sys_select_once((int)SYSCALL_ARG0(frame), SYSCALL_ARG1(frame),
                                                       SYSCALL_ARG2(frame), SYSCALL_ARG3(frame), 1);
            }
            break;
        }
        case SYS_SCHED_YIELD: SYSCALL_RET(frame) = 0; process_yield_from_syscall(frame); break;
        case SYS_SCHED_GETAFFINITY:
            SYSCALL_RET(frame) = (uint64_t)sys_sched_getaffinity(SYSCALL_ARG0(frame),
                                                        (size_t)SYSCALL_ARG1(frame), SYSCALL_ARG2(frame));
            break;
        case SYS_SCHED_SETAFFINITY:
            SYSCALL_RET(frame) = (uint64_t)sys_sched_setaffinity(SYSCALL_ARG0(frame),
                                                        (size_t)SYSCALL_ARG1(frame), SYSCALL_ARG2(frame));
            if ((int64_t)SYSCALL_RET(frame) == 0) {
                struct cpu_mask mask;
                if (process_get_affinity(0, &mask) == 0 &&
                    !cpu_mask_test(&mask, cpu_current()->index))
                    process_yield_from_syscall(frame);
            }
            break;
        case SYS_EPOLL_CREATE:
            SYSCALL_RET(frame) = SYSCALL_ARG0(frame) == 0 ? (uint64_t)-(int64_t)EINVAL :
                         (uint64_t)sys_epoll_create(0);
            break;
        case SYS_DUP: SYSCALL_RET(frame) = (uint64_t)sys_dup((int)SYSCALL_ARG0(frame), 0, 0); break;
        case SYS_DUP2: SYSCALL_RET(frame) = (uint64_t)sys_dup_to((int)SYSCALL_ARG0(frame), (int)SYSCALL_ARG1(frame), 0, 0); break;
        case SYS_DUP3:
            if (SYSCALL_ARG2(frame) & ~O_CLOEXEC) SYSCALL_RET(frame) = (uint64_t)-(int64_t)EINVAL;
            else SYSCALL_RET(frame) = (uint64_t)sys_dup_to((int)SYSCALL_ARG0(frame), (int)SYSCALL_ARG1(frame),
                                                   (SYSCALL_ARG2(frame) & O_CLOEXEC) != 0, 1);
            break;
        case SYS_NANOSLEEP: {
            struct linux_timespec request;
            if (copy_from_user(&request, SYSCALL_ARG0(frame), sizeof(request)) != 0) {
                SYSCALL_RET(frame) = (uint64_t)-(int64_t)EFAULT;
                break;
            }
            if (request.tv_sec < 0 || request.tv_nsec < 0 || request.tv_nsec >= 1000000000LL) {
                SYSCALL_RET(frame) = (uint64_t)-(int64_t)EINVAL;
                break;
            }
            int64_t duration = request.tv_sec > (INT64_MAX - request.tv_nsec) / 1000000000LL ?
                INT64_MAX : request.tv_sec * 1000000000LL + request.tv_nsec;
            if (duration == 0) SYSCALL_RET(frame) = 0;
            else {
                io_watch_begin(process_current());
                if (!retry_io_wait(frame, SYS_NANOSLEEP, duration)) SYSCALL_RET(frame) = 0;
            }
            break;
        }
        case SYS_GETITIMER: {
            struct process *process = process_current();
            if ((int)SYSCALL_ARG0(frame) != 0 ) {
                SYSCALL_RET(frame) = (uint64_t)-(int64_t)EINVAL;
                break;
            }
            if (!process) {
                SYSCALL_RET(frame) = (uint64_t)-(int64_t)EINVAL;
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
            if (SYSCALL_ARG1(frame) && copy_to_user(SYSCALL_ARG1(frame), current_value, sizeof(current_value)) != 0) {
                SYSCALL_RET(frame) = (uint64_t)-(int64_t)EFAULT;
                break;
            }
            SYSCALL_RET(frame) = 0;
            break;
        }
        case SYS_SETITIMER: {
            struct process *process = process_current();
            if ((int)SYSCALL_ARG0(frame) != 0 ) {
                SYSCALL_RET(frame) = (uint64_t)-(int64_t)EINVAL;
                break;
            }
            if (!process || !SYSCALL_ARG1(frame)) {
                SYSCALL_RET(frame) = (uint64_t)-(int64_t)EFAULT;
                break;
            }
            struct linux_timeval new_value[2];
            if (copy_from_user(new_value, SYSCALL_ARG1(frame), sizeof(new_value)) != 0) {
                SYSCALL_RET(frame) = (uint64_t)-(int64_t)EFAULT;
                break;
            }
            if (new_value[0].tv_sec < 0 || new_value[0].tv_usec < 0 || new_value[0].tv_usec >= 1000000LL ||
                new_value[1].tv_sec < 0 || new_value[1].tv_usec < 0 || new_value[1].tv_usec >= 1000000LL) {
                SYSCALL_RET(frame) = (uint64_t)-(int64_t)EINVAL;
                break;
            }
            uint64_t now = time_uptime_ns();
            if (SYSCALL_ARG2(frame)) {
                uint64_t remaining_ns = process->itimer_real_deadline_ns > now ?
                    process->itimer_real_deadline_ns - now : 0;
                struct linux_timeval old_value[2];
                old_value[0].tv_sec = (int64_t)(process->itimer_real_interval_ns / 1000000000ULL);
                old_value[0].tv_usec = (int64_t)((process->itimer_real_interval_ns / 1000ULL) % 1000000ULL);
                old_value[1].tv_sec = (int64_t)(remaining_ns / 1000000000ULL);
                old_value[1].tv_usec = (int64_t)((remaining_ns / 1000ULL) % 1000000ULL);
                if (copy_to_user(SYSCALL_ARG2(frame), old_value, sizeof(old_value)) != 0) {
                    SYSCALL_RET(frame) = (uint64_t)-(int64_t)EFAULT;
                    break;
                }
            }
            uint64_t interval_ns = (uint64_t)new_value[0].tv_sec * 1000000000ULL +
                                    (uint64_t)new_value[0].tv_usec * 1000ULL;
            uint64_t value_ns = (uint64_t)new_value[1].tv_sec * 1000000000ULL +
                                (uint64_t)new_value[1].tv_usec * 1000ULL;
            process->itimer_real_interval_ns = interval_ns;
            process->itimer_real_deadline_ns = value_ns ? now + value_ns : 0;
            process_note_deadline(process->itimer_real_deadline_ns);
            SYSCALL_RET(frame) = 0;
            break;
        }
        case SYS_ALARM: {
            struct process *process = process_current();
            if (!process) {
                SYSCALL_RET(frame) = 0;
                break;
            }
            uint64_t seconds = SYSCALL_ARG0(frame) & 0xFFFFFFFFULL;
            uint64_t now = time_uptime_ns();
            uint64_t remaining_ns = process->itimer_real_deadline_ns > now ?
                process->itimer_real_deadline_ns - now : 0;
            uint64_t remaining_sec = remaining_ns / 1000000000ULL;
            if (remaining_ns % 1000000000ULL) remaining_sec++;
            process->itimer_real_interval_ns = 0;
            process->itimer_real_deadline_ns = seconds ? now + seconds * 1000000000ULL : 0;
            process_note_deadline(process->itimer_real_deadline_ns);
            SYSCALL_RET(frame) = remaining_sec;
            break;
        }
        case SYS_EPOLL_WAIT:
        case SYS_EPOLL_PWAIT: {
            int timeout_ms = (int)SYSCALL_ARG3(frame);
            int64_t timeout_ns = timeout_ms_to_ns(timeout_ms);
            int64_t result = sys_epoll_wait_once((int)SYSCALL_ARG0(frame), SYSCALL_ARG1(frame),
                                                  (int)SYSCALL_ARG2(frame), timeout_ms == 0);
            if (result != 0 || timeout_ms == 0) {
                clear_io_wait(process_current());
                SYSCALL_RET(frame) = (uint64_t)result;
            } else if (!retry_io_wait(frame, syscall_number, timeout_ns)) {
                SYSCALL_RET(frame) = (uint64_t)sys_epoll_wait_once((int)SYSCALL_ARG0(frame), SYSCALL_ARG1(frame),
                                                            (int)SYSCALL_ARG2(frame), 1);
            }
            break;
        }
        case SYS_EPOLL_CTL:
            SYSCALL_RET(frame) = (uint64_t)sys_epoll_ctl((int)SYSCALL_ARG0(frame), (int)SYSCALL_ARG1(frame),
                                                 (int)SYSCALL_ARG2(frame), SYSCALL_ARG3(frame));
            break;
        case SYS_SOCKET: SYSCALL_RET(frame) = (uint64_t)sys_socket((int)SYSCALL_ARG0(frame), (int)SYSCALL_ARG1(frame), (int)SYSCALL_ARG2(frame)); break;
        case SYS_CONNECT: {
            int fd = (int)SYSCALL_ARG0(frame);
            int64_t result = sys_connect(fd, SYSCALL_ARG1(frame), SYSCALL_ARG2(frame));
            struct process *process = process_current();
            struct file *file = process && fd >= 0 && fd < PROCESS_FD_CAPACITY(process) ? fd_file(fd) : NULL;

            if (result == -EINPROGRESS && file && file->kind == FILE_KIND_INET_SOCKET &&
                !(file->flags & O_NONBLOCK)) {
                block_and_retry(frame, SYS_CONNECT, file, 1);
            } else {
                SYSCALL_RET(frame) = (uint64_t)result;
            }
            break;
        }
        case SYS_ACCEPT:
            accept_or_block(frame, SYS_ACCEPT, (int)SYSCALL_ARG0(frame), SYSCALL_ARG1(frame), SYSCALL_ARG2(frame), 0);
            break;
        case SYS_SENDTO: SYSCALL_RET(frame) = (uint64_t)sys_sendto((int)SYSCALL_ARG0(frame), SYSCALL_ARG1(frame), SYSCALL_ARG2(frame), (int)SYSCALL_ARG3(frame), SYSCALL_ARG4(frame), SYSCALL_ARG5(frame)); break;
        case SYS_RECVFROM: {
            int fd = (int)SYSCALL_ARG0(frame);
            int flags = (int)SYSCALL_ARG3(frame);
            int64_t result = sys_recvfrom(fd, SYSCALL_ARG1(frame), SYSCALL_ARG2(frame), flags, SYSCALL_ARG4(frame), SYSCALL_ARG5(frame));
            struct process *process = process_current();
            struct file *file = process && fd >= 0 && fd < PROCESS_FD_CAPACITY(process) ? fd_file(fd) : NULL;
            if (result == -EAGAIN && file && !(file->flags & O_NONBLOCK) && !(flags & MSG_DONTWAIT)) {
                block_and_retry(frame, SYS_RECVFROM, file, 0);
            } else {
                SYSCALL_RET(frame) = (uint64_t)result;
            }
            break;
        }
        case SYS_SENDMSG: SYSCALL_RET(frame) = (uint64_t)sys_sendmsg((int)SYSCALL_ARG0(frame), SYSCALL_ARG1(frame), (int)SYSCALL_ARG2(frame)); break;
        case SYS_SENDMMSG: SYSCALL_RET(frame) = (uint64_t)sys_sendmmsg((int)SYSCALL_ARG0(frame), SYSCALL_ARG1(frame), (unsigned)SYSCALL_ARG2(frame), (int)SYSCALL_ARG3(frame)); break;
        case SYS_RECVMMSG: {
            int fd = (int)SYSCALL_ARG0(frame);
            int flags = (int)SYSCALL_ARG3(frame);
            int64_t result = sys_recvmmsg(fd, SYSCALL_ARG1(frame), (unsigned)SYSCALL_ARG2(frame), flags);
            struct process *process = process_current();
            struct file *file = process && fd >= 0 && fd < PROCESS_FD_CAPACITY(process) ? fd_file(fd) : NULL;
            if (result == -EAGAIN && file && !(file->flags & O_NONBLOCK) && !(flags & MSG_DONTWAIT)) {
                block_and_retry(frame, SYS_RECVMMSG, file, 0);
            } else {
                SYSCALL_RET(frame) = (uint64_t)result;
            }
            break;
        }
        case SYS_RECVMSG: {
            int fd = (int)SYSCALL_ARG0(frame);
            int flags = (int)SYSCALL_ARG2(frame);
            int64_t result = sys_recvmsg(fd, SYSCALL_ARG1(frame), flags);
            struct process *process = process_current();
            struct file *file = process && fd >= 0 && fd < PROCESS_FD_CAPACITY(process) ? fd_file(fd) : NULL;
            if (result == -EAGAIN && file && !(file->flags & O_NONBLOCK) && !(flags & MSG_DONTWAIT)) {
                block_and_retry(frame, SYS_RECVMSG, file, 0);
            } else {
                SYSCALL_RET(frame) = (uint64_t)result;
            }
            break;
        }
        case SYS_SHUTDOWN: SYSCALL_RET(frame) = (uint64_t)sys_shutdown((int)SYSCALL_ARG0(frame), (int)SYSCALL_ARG1(frame)); break;
        case SYS_BIND: SYSCALL_RET(frame) = (uint64_t)sys_bind((int)SYSCALL_ARG0(frame), SYSCALL_ARG1(frame), SYSCALL_ARG2(frame)); break;
        case SYS_LISTEN: SYSCALL_RET(frame) = (uint64_t)sys_listen((int)SYSCALL_ARG0(frame), (int)SYSCALL_ARG1(frame)); break;
        case SYS_GETSOCKNAME: SYSCALL_RET(frame) = (uint64_t)sys_socket_name((int)SYSCALL_ARG0(frame), SYSCALL_ARG1(frame), SYSCALL_ARG2(frame), 0); break;
        case SYS_GETPEERNAME: SYSCALL_RET(frame) = (uint64_t)sys_socket_name((int)SYSCALL_ARG0(frame), SYSCALL_ARG1(frame), SYSCALL_ARG2(frame), 1); break;
        case SYS_SOCKETPAIR: SYSCALL_RET(frame) = (uint64_t)sys_socketpair((int)SYSCALL_ARG0(frame), (int)SYSCALL_ARG1(frame), (int)SYSCALL_ARG2(frame), SYSCALL_ARG3(frame)); break;
        case SYS_SETSOCKOPT: SYSCALL_RET(frame) = (uint64_t)sys_setsockopt((int)SYSCALL_ARG0(frame), (int)SYSCALL_ARG1(frame), (int)SYSCALL_ARG2(frame), SYSCALL_ARG3(frame), SYSCALL_ARG4(frame)); break;
        case SYS_GETSOCKOPT: SYSCALL_RET(frame) = (uint64_t)sys_getsockopt((int)SYSCALL_ARG0(frame), (int)SYSCALL_ARG1(frame), (int)SYSCALL_ARG2(frame), SYSCALL_ARG3(frame), SYSCALL_ARG4(frame)); break;
        case SYS_GETPID: SYSCALL_RET(frame) = process_current_pid(); break;
        case SYS_GETTID: SYSCALL_RET(frame) = process_current_tid(); break;
        case SYS_CLONE: {
            int64_t pid = sys_clone_fork_compat(
                frame, SYSCALL_ARG0(frame), SYSCALL_ARG1(frame), SYSCALL_ARG2(frame),
                SYSCALL_CLONE_CHILD_TID(frame), SYSCALL_CLONE_TLS(frame), 0);
            SYSCALL_RET(frame) = (uint64_t)pid;
            if (pid > 0) process_run_child_first_from_syscall(frame, (uint64_t)pid);
            break;
        }
        case SYS_CLONE3: {
            int64_t pid = sys_clone3_fork_compat(frame, SYSCALL_ARG0(frame), (size_t)SYSCALL_ARG1(frame));
            SYSCALL_RET(frame) = (uint64_t)pid;
            if (pid > 0) process_run_child_first_from_syscall(frame, (uint64_t)pid);
            break;
        }
        case SYS_FORK:
        case SYS_VFORK: {
            int64_t pid = process_fork_from_syscall(frame, NULL);
            SYSCALL_RET(frame) = (uint64_t)pid;
            if (pid > 0) process_run_child_first_from_syscall(frame, (uint64_t)pid);
            break;
        }
        case SYS_EXECVE: SYSCALL_RET(frame) = (uint64_t)sys_execve(frame, SYSCALL_ARG0(frame), SYSCALL_ARG1(frame), SYSCALL_ARG2(frame)); break;
        case SYS_EXIT: process_exit_from_syscall(frame, (int)SYSCALL_ARG0(frame)); break;
        case SYS_EXIT_GROUP: process_exit_group_from_syscall(frame, (int)SYSCALL_ARG0(frame)); break;
        case SYS_WAIT4: {
            int64_t result = process_waitpid_from_syscall(frame, (int64_t)SYSCALL_ARG0(frame), SYSCALL_ARG1(frame), (int)SYSCALL_ARG2(frame), SYS_WAIT4);
            if (result != PROCESS_RESTARTED && process_current() == caller)
                SYSCALL_RET(frame) = (uint64_t)result;
            break;
        }
        case SYS_WAITID: {
            int idtype = (int)SYSCALL_ARG0(frame);
            int64_t id = (int64_t)SYSCALL_ARG1(frame);
            int options = (int)SYSCALL_ARG3(frame);
            int64_t pid_spec;
            if (idtype == 0) pid_spec = -1;
            else if (idtype == 1 && id > 0) pid_spec = id;
            else if (idtype == 2 && id > 0) pid_spec = -id;
            else { SYSCALL_RET(frame) = (uint64_t)-(int64_t)EINVAL; break; }
            options &= ~(WNOTHREAD | WALLCHILDREN | WCLONE);
            if ((options & ~(WNOHANG | WNOWAIT | WEXITED | WSTOPPED | WCONTINUED)) ||
                !(options & (WEXITED | WSTOPPED | WCONTINUED))) {
                SYSCALL_RET(frame) = (uint64_t)-(int64_t)EINVAL;
                break;
            }
            int64_t result = process_waitid_from_syscall(pid_spec, SYSCALL_ARG2(frame),
                                                         options);
            if (result == -EAGAIN) {
                if (!retry_io_wait(frame, SYS_WAITID, -1))
                    SYSCALL_RET(frame) = (uint64_t)-(int64_t)ECHILD;
            } else {
                clear_io_wait(process_current());
                SYSCALL_RET(frame) = (uint64_t)result;
            }
            break;
        }
        case SYS_KILL: SYSCALL_RET(frame) = (uint64_t)process_send_signal_checked((int64_t)SYSCALL_ARG0(frame), (int)SYSCALL_ARG1(frame)); break;

        case SYS_TKILL: SYSCALL_RET(frame) = (uint64_t)process_send_signal_checked((int64_t)SYSCALL_ARG0(frame), (int)SYSCALL_ARG1(frame)); break;
        case SYS_TGKILL: SYSCALL_RET(frame) = (uint64_t)process_send_signal_checked((int64_t)SYSCALL_ARG1(frame), (int)SYSCALL_ARG2(frame)); break;
        case SYS_UNAME: SYSCALL_RET(frame) = (uint64_t)sys_uname(SYSCALL_ARG0(frame)); break;
        case SYS_SYSLOG:
            SYSCALL_RET(frame) = (uint64_t)sys_syslog((int)SYSCALL_ARG0(frame), SYSCALL_ARG1(frame),
                                              (int)SYSCALL_ARG2(frame));
            break;
        case SYS_SETHOSTNAME:
            SYSCALL_RET(frame) = (uint64_t)set_machine_name(0, SYSCALL_ARG0(frame), SYSCALL_ARG1(frame));
            break;
        case SYS_SETDOMAINNAME:
            SYSCALL_RET(frame) = (uint64_t)set_machine_name(1, SYSCALL_ARG0(frame), SYSCALL_ARG1(frame));
            break;
        case SYS_FCNTL: {
            struct process *process = process_current();
            int fd = (int)SYSCALL_ARG0(frame);
            int command = (int)SYSCALL_ARG1(frame);
            if (!process || fd < 0 || fd >= PROCESS_FD_CAPACITY(process) || !fd_file(fd)) {
                SYSCALL_RET(frame) = (uint64_t)-(int64_t)EBADF;
            } else if (command == F_DUPFD || command == F_DUPFD_CLOEXEC) {
                SYSCALL_RET(frame) = (uint64_t)sys_dup(fd, (int)SYSCALL_ARG2(frame),
                    command == F_DUPFD_CLOEXEC);
            } else if (command == F_GETFD) {
                SYSCALL_RET(frame) = (process_get_fd_flags(process, fd) & PROCESS_FD_CLOEXEC) ? FD_CLOEXEC : 0;
            } else if (command == F_SETFD) {
                if (SYSCALL_ARG2(frame) & ~(uint64_t)FD_CLOEXEC)
                    SYSCALL_RET(frame) = (uint64_t)-(int64_t)EINVAL;
                else {
                    process_set_fd_flags(process, fd,
                        (SYSCALL_ARG2(frame) & FD_CLOEXEC) ? PROCESS_FD_CLOEXEC : 0);
                    SYSCALL_RET(frame) = 0;
                }
            } else if (command == F_GETLK || command == F_SETLK || command == F_SETLKW) {
                SYSCALL_RET(frame) = (uint64_t)sys_fcntl_lock(fd, command, SYSCALL_ARG2(frame));
            } else if (command == F_SETPIPE_SZ || command == F_GETPIPE_SZ) {
                struct file *file = fd_file(fd);
                SYSCALL_RET(frame) = (uint64_t)pipe_size_control(file, command == F_SETPIPE_SZ,
                                                                 SYSCALL_ARG2(frame));
            } else if (command == F_ADD_SEALS || command == F_GET_SEALS) {
                struct file *file = fd_file(fd);
                if (file->kind != FILE_KIND_MEMFD) {
                    SYSCALL_RET(frame) = (uint64_t)-(int64_t)EINVAL;
                } else if (command == F_GET_SEALS) {
                    SYSCALL_RET(frame) = memfd_seals(file->memfd);
                } else {
                    SYSCALL_RET(frame) = (uint64_t)(int64_t)
                        memfd_add_seals(file->memfd, (uint32_t)SYSCALL_ARG2(frame));
                }
            } else if (command == F_GETFL) {
                SYSCALL_RET(frame) = SYSCALL_OPEN_FLAGS_OUT(fd_file(fd)->flags);
            } else if (command == F_SETFL) {
                struct file *file = fd_file(fd);
                uint32_t wanted = (uint32_t)SYSCALL_ARG2(frame) & (uint32_t)O_NONBLOCK;
                if (wanted) __atomic_fetch_or(&file->flags, (uint32_t)O_NONBLOCK, __ATOMIC_RELAXED);
                else __atomic_fetch_and(&file->flags, ~(uint32_t)O_NONBLOCK, __ATOMIC_RELAXED);
                SYSCALL_RET(frame) = 0;
            } else {
                SYSCALL_RET(frame) = (uint64_t)-(int64_t)EINVAL;
            }
            break;
        }
        case SYS_FSYNC: SYSCALL_RET(frame) = (uint64_t)sys_fsync((int)SYSCALL_ARG0(frame)); break;
        case SYS_FDATASYNC: SYSCALL_RET(frame) = (uint64_t)sys_fsync((int)SYSCALL_ARG0(frame)); break;
        case SYS_SYNCFS: SYSCALL_RET(frame) = (uint64_t)sys_fsync((int)SYSCALL_ARG0(frame)); break;
        case SYS_SYNC: SYSCALL_RET(frame) = (uint64_t)vfs_sync(); break;
        case SYS_INIT_MODULE:
            SYSCALL_RET(frame) = (uint64_t)sys_init_module(SYSCALL_ARG0(frame),
                                                           SYSCALL_ARG1(frame),
                                                           SYSCALL_ARG2(frame));
            break;
        case SYS_FINIT_MODULE:
            SYSCALL_RET(frame) = (uint64_t)sys_finit_module((int)SYSCALL_ARG0(frame),
                                                            SYSCALL_ARG1(frame),
                                                            (int)SYSCALL_ARG2(frame));
            break;
        case SYS_DELETE_MODULE:
            SYSCALL_RET(frame) = (uint64_t)sys_delete_module(SYSCALL_ARG0(frame),
                                                             (uint32_t)SYSCALL_ARG1(frame));
            break;
        case SYS_REBOOT:
            SYSCALL_RET(frame) = (uint64_t)sys_reboot((uint32_t)SYSCALL_ARG0(frame),
                                              (uint32_t)SYSCALL_ARG1(frame),
                                              (uint32_t)SYSCALL_ARG2(frame));
            break;
        case SYS_FTRUNCATE: SYSCALL_RET(frame) = (uint64_t)sys_ftruncate((int)SYSCALL_ARG0(frame), SYSCALL_ARG1(frame)); break;
        case SYS_FLOCK: {
            int operation = (int)SYSCALL_ARG1(frame);
            int64_t result = sys_flock((int)SYSCALL_ARG0(frame), operation);

            if (result == -EAGAIN && !(operation & FILE_LOCK_NB)) {
                if (!retry_io_wait(frame, SYS_FLOCK, -1))
                    SYSCALL_RET(frame) = (uint64_t)result;
            } else {
                clear_io_wait(process_current());
                SYSCALL_RET(frame) = (uint64_t)result;
            }
            break;
        }
        case SYS_MEMFD_CREATE:
            SYSCALL_RET(frame) = (uint64_t)sys_memfd_create(SYSCALL_ARG0(frame), (uint32_t)SYSCALL_ARG1(frame));
            break;
        case SYS_FALLOCATE:
            SYSCALL_RET(frame) = (uint64_t)sys_fallocate((int)SYSCALL_ARG0(frame), (int)SYSCALL_ARG1(frame),
                                                 SYSCALL_ARG2(frame), SYSCALL_ARG3(frame));
            break;

        case SYS_SIGNALFD:
            SYSCALL_RET(frame) = (uint64_t)sys_signalfd((int)SYSCALL_ARG0(frame), SYSCALL_ARG1(frame),
                                                SYSCALL_ARG2(frame), 0);
            break;
        case SYS_SIGNALFD4:
            SYSCALL_RET(frame) = (uint64_t)sys_signalfd((int)SYSCALL_ARG0(frame), SYSCALL_ARG1(frame),
                                                SYSCALL_ARG2(frame), (int)SYSCALL_ARG3(frame));
            break;
        case SYS_GETCWD: SYSCALL_RET(frame) = (uint64_t)sys_getcwd(SYSCALL_ARG0(frame), (size_t)SYSCALL_ARG1(frame)); break;
        case SYS_CHDIR: SYSCALL_RET(frame) = (uint64_t)sys_chdir(SYSCALL_ARG0(frame)); break;
        case SYS_FCHDIR: SYSCALL_RET(frame) = (uint64_t)sys_fchdir((int)SYSCALL_ARG0(frame)); break;
        case SYS_CHROOT: SYSCALL_RET(frame) = (uint64_t)sys_chroot(SYSCALL_ARG0(frame)); break;
        case SYS_SETXATTR:
        case SYS_LSETXATTR: {
            int64_t status = xattr_target_exists(SYSCALL_ARG0(frame), syscall_number == SYS_SETXATTR);
            SYSCALL_RET(frame) = (uint64_t)(status != 0 ? status : -(int64_t)EOPNOTSUPP);
            break;
        }
        case SYS_FSETXATTR: {
            int64_t status = xattr_descriptor_exists((int)SYSCALL_ARG0(frame));
            SYSCALL_RET(frame) = (uint64_t)(status != 0 ? status : -(int64_t)EOPNOTSUPP);
            break;
        }
        case SYS_GETXATTR:
        case SYS_LGETXATTR:
        case SYS_REMOVEXATTR:
        case SYS_LREMOVEXATTR: {
            int follow = syscall_number == SYS_GETXATTR || syscall_number == SYS_REMOVEXATTR;
            int64_t status = xattr_target_exists(SYSCALL_ARG0(frame), follow);
            SYSCALL_RET(frame) = (uint64_t)(status != 0 ? status : -(int64_t)ENODATA);
            break;
        }
        case SYS_FGETXATTR:
        case SYS_FREMOVEXATTR: {
            int64_t status = xattr_descriptor_exists((int)SYSCALL_ARG0(frame));
            SYSCALL_RET(frame) = (uint64_t)(status != 0 ? status : -(int64_t)ENODATA);
            break;
        }
        case SYS_LISTXATTR:
        case SYS_LLISTXATTR: {
            int64_t status = xattr_target_exists(SYSCALL_ARG0(frame), syscall_number == SYS_LISTXATTR);
            SYSCALL_RET(frame) = (uint64_t)status;
            break;
        }
        case SYS_FLISTXATTR:
            SYSCALL_RET(frame) = (uint64_t)xattr_descriptor_exists((int)SYSCALL_ARG0(frame));
            break;
        case SYS_RENAME: SYSCALL_RET(frame) = (uint64_t)sys_rename_at(AT_FDCWD, SYSCALL_ARG0(frame), AT_FDCWD, SYSCALL_ARG1(frame), 0); break;
        case SYS_MKDIR: SYSCALL_RET(frame) = (uint64_t)sys_mkdir_at(AT_FDCWD, SYSCALL_ARG0(frame), SYSCALL_ARG1(frame)); break;
        case SYS_RMDIR: SYSCALL_RET(frame) = (uint64_t)sys_unlink_at(AT_FDCWD, SYSCALL_ARG0(frame), AT_REMOVEDIR); break;
        case SYS_UNLINK: SYSCALL_RET(frame) = (uint64_t)sys_unlink_at(AT_FDCWD, SYSCALL_ARG0(frame), 0); break;
        case SYS_READLINK: SYSCALL_RET(frame) = (uint64_t)sys_readlink_at(AT_FDCWD, SYSCALL_ARG0(frame), SYSCALL_ARG1(frame), (size_t)SYSCALL_ARG2(frame)); break;
        case SYS_CHMOD: SYSCALL_RET(frame) = (uint64_t)sys_chmod_at(AT_FDCWD, SYSCALL_ARG0(frame), (uint32_t)SYSCALL_ARG1(frame), 0); break;
        case SYS_FCHMOD: SYSCALL_RET(frame) = (uint64_t)sys_fchmod((int)SYSCALL_ARG0(frame), (uint32_t)SYSCALL_ARG1(frame)); break;
        case SYS_CHOWN:
            SYSCALL_RET(frame) = (uint64_t)sys_chown_at(AT_FDCWD, SYSCALL_ARG0(frame), (uint32_t)SYSCALL_ARG1(frame),
                                                (uint32_t)SYSCALL_ARG2(frame), 0);
            break;
        case SYS_LCHOWN:
            SYSCALL_RET(frame) = (uint64_t)sys_chown_at(AT_FDCWD, SYSCALL_ARG0(frame), (uint32_t)SYSCALL_ARG1(frame),
                                                (uint32_t)SYSCALL_ARG2(frame), AT_SYMLINK_NOFOLLOW);
            break;
        case SYS_FCHOWN:
            SYSCALL_RET(frame) = (uint64_t)sys_fchown((int)SYSCALL_ARG0(frame), (uint32_t)SYSCALL_ARG1(frame),
                                              (uint32_t)SYSCALL_ARG2(frame));
            break;
        case SYS_FCHOWNAT:
            SYSCALL_RET(frame) = (uint64_t)sys_chown_at((int)SYSCALL_ARG0(frame), SYSCALL_ARG1(frame), (uint32_t)SYSCALL_ARG2(frame),
                                                (uint32_t)SYSCALL_ARG3(frame), (int)SYSCALL_ARG4(frame));
            break;
        case SYS_UMASK: SYSCALL_RET(frame) = process_set_umask((uint32_t)SYSCALL_ARG0(frame)); break;
        case SYS_GETTIMEOFDAY: SYSCALL_RET(frame) = (uint64_t)sys_gettimeofday(SYSCALL_ARG0(frame)); break;
        case SYS_GETRLIMIT: SYSCALL_RET(frame) = (uint64_t)sys_prlimit(0, SYSCALL_ARG0(frame), 0, SYSCALL_ARG1(frame)); break;
        case SYS_SETRLIMIT: SYSCALL_RET(frame) = (uint64_t)sys_prlimit(0, SYSCALL_ARG0(frame), SYSCALL_ARG1(frame), 0); break;

        case SYS_GETPRIORITY: {
            int nice = 0;
            SYSCALL_RET(frame) = process_get_nice(SYSCALL_ARG0(frame) == 0 ? SYSCALL_ARG1(frame) : 0, &nice) == 0
                             ? (uint64_t)(20 - nice) : 20;
            break;
        }
        case SYS_SETPRIORITY: {
            int result = process_set_nice(SYSCALL_ARG0(frame) == 0 ? SYSCALL_ARG1(frame) : 0,
                                          (int)(int32_t)SYSCALL_ARG2(frame));
            SYSCALL_RET(frame) = result == 0 ? 0 : (uint64_t)(int64_t)result;
            break;
        }
        case SYS_GETRUSAGE: SYSCALL_RET(frame) = (uint64_t)sys_getrusage(SYSCALL_ARG1(frame)); break;

        case SYS_SCHED_GETSCHEDULER: {
            int policy = 0;
            int result = process_get_scheduler(SYSCALL_ARG0(frame), &policy, NULL);
            SYSCALL_RET(frame) = result == 0 ? (uint64_t)policy : (uint64_t)(int64_t)result;
            break;
        }
        case SYS_SCHED_SETSCHEDULER: {
            uint32_t priority = 0;
            if (SYSCALL_ARG2(frame) &&
                copy_from_user(&priority, SYSCALL_ARG2(frame), sizeof(priority)) != 0) {
                SYSCALL_RET(frame) = (uint64_t)-(int64_t)EFAULT;
                break;
            }
            int result = process_set_scheduler(SYSCALL_ARG0(frame), (int)SYSCALL_ARG1(frame),
                                               (int)priority);
            SYSCALL_RET(frame) = result == 0 ? 0 : (uint64_t)(int64_t)result;
            break;
        }
        case SYS_SCHED_GETPARAM:
        case SYS_SCHED_SETPARAM: {
            uint32_t priority = 0;
            if (syscall_number == SYS_SCHED_SETPARAM) {
                if (copy_from_user(&priority, SYSCALL_ARG1(frame), sizeof(priority)) != 0) {
                    SYSCALL_RET(frame) = (uint64_t)-(int64_t)EFAULT;
                    break;
                }

                int policy = PROCESS_SCHED_OTHER;
                int result = process_get_scheduler(SYSCALL_ARG0(frame), &policy, NULL);
                if (result == 0)
                    result = process_set_scheduler(SYSCALL_ARG0(frame), policy, (int)priority);
                SYSCALL_RET(frame) = result == 0 ? 0 : (uint64_t)(int64_t)result;
                break;
            }
            int stored = 0;
            int result = process_get_scheduler(SYSCALL_ARG0(frame), NULL, &stored);
            if (result != 0) {
                SYSCALL_RET(frame) = (uint64_t)(int64_t)result;
                break;
            }
            priority = (uint32_t)stored;
            SYSCALL_RET(frame) = copy_to_user(SYSCALL_ARG1(frame), &priority, sizeof(priority)) == 0
                             ? 0 : (uint64_t)-(int64_t)EFAULT;
            break;
        }

        case SYS_SCHED_GET_PRIORITY_MAX:
            SYSCALL_RET(frame) = (SYSCALL_ARG0(frame) == PROCESS_SCHED_FIFO ||
                          SYSCALL_ARG0(frame) == PROCESS_SCHED_RR)
                             ? PROCESS_RT_PRIORITY_MAX : 0;
            break;
        case SYS_SCHED_GET_PRIORITY_MIN:
            SYSCALL_RET(frame) = (SYSCALL_ARG0(frame) == PROCESS_SCHED_FIFO ||
                          SYSCALL_ARG0(frame) == PROCESS_SCHED_RR) ? 1 : 0;
            break;
        case SYS_SCHED_RR_GET_INTERVAL: {
            struct { int64_t seconds; int64_t nanoseconds; } slice = {0, 0};
            SYSCALL_RET(frame) = copy_to_user(SYSCALL_ARG1(frame), &slice, sizeof(slice)) == 0
                ? 0 : (uint64_t)-(int64_t)EFAULT;
            break;
        }
        case SYS_GETCPU: {
            uint32_t cpu = cpu_current() ? cpu_current()->index : 0;
            uint32_t node = 0;
            if (SYSCALL_ARG0(frame) && copy_to_user(SYSCALL_ARG0(frame), &cpu, sizeof(cpu)) != 0)
                SYSCALL_RET(frame) = (uint64_t)-(int64_t)EFAULT;
            else if (SYSCALL_ARG1(frame) && copy_to_user(SYSCALL_ARG1(frame), &node, sizeof(node)) != 0)
                SYSCALL_RET(frame) = (uint64_t)-(int64_t)EFAULT;
            else SYSCALL_RET(frame) = 0;
            break;
        }

        case SYS_MEMBARRIER: SYSCALL_RET(frame) = 0; break;
        case SYS_SYSINFO: SYSCALL_RET(frame) = (uint64_t)sys_sysinfo(SYSCALL_ARG0(frame)); break;

        case SYS_TIME: {
            int64_t seconds = (int64_t)time_epoch_seconds();
            if (SYSCALL_ARG0(frame) && copy_to_user(SYSCALL_ARG0(frame), &seconds, sizeof(seconds)) != 0)
                SYSCALL_RET(frame) = (uint64_t)-(int64_t)EFAULT;
            else SYSCALL_RET(frame) = (uint64_t)seconds;
            break;
        }
        case SYS_TIMES: SYSCALL_RET(frame) = (uint64_t)sys_times(SYSCALL_ARG0(frame)); break;
        case SYS_GETUID: SYSCALL_RET(frame) = cred_current() ? cred_current()->uid : 0; break;
        case SYS_GETGID: SYSCALL_RET(frame) = cred_current() ? cred_current()->gid : 0; break;
        case SYS_GETEUID: SYSCALL_RET(frame) = cred_current() ? cred_current()->euid : 0; break;
        case SYS_GETEGID: SYSCALL_RET(frame) = cred_current() ? cred_current()->egid : 0; break;
        case SYS_SETUID: SYSCALL_RET(frame) = (uint64_t)cred_set_uid((uint32_t)SYSCALL_ARG0(frame)); break;
        case SYS_SETGID: SYSCALL_RET(frame) = (uint64_t)cred_set_gid((uint32_t)SYSCALL_ARG0(frame)); break;
        case SYS_SETREUID:
            SYSCALL_RET(frame) = (uint64_t)cred_set_reuid((uint32_t)SYSCALL_ARG0(frame), (uint32_t)SYSCALL_ARG1(frame));
            break;
        case SYS_SETREGID:
            SYSCALL_RET(frame) = (uint64_t)cred_set_regid((uint32_t)SYSCALL_ARG0(frame), (uint32_t)SYSCALL_ARG1(frame));
            break;
        case SYS_SETRESUID:
            SYSCALL_RET(frame) = (uint64_t)cred_set_resuid((uint32_t)SYSCALL_ARG0(frame), (uint32_t)SYSCALL_ARG1(frame),
                                                   (uint32_t)SYSCALL_ARG2(frame));
            break;
        case SYS_SETRESGID:
            SYSCALL_RET(frame) = (uint64_t)cred_set_resgid((uint32_t)SYSCALL_ARG0(frame), (uint32_t)SYSCALL_ARG1(frame),
                                                   (uint32_t)SYSCALL_ARG2(frame));
            break;
        case SYS_GETRESUID: SYSCALL_RET(frame) = (uint64_t)sys_getresuid(SYSCALL_ARG0(frame), SYSCALL_ARG1(frame), SYSCALL_ARG2(frame), 0); break;
        case SYS_GETRESGID: SYSCALL_RET(frame) = (uint64_t)sys_getresuid(SYSCALL_ARG0(frame), SYSCALL_ARG1(frame), SYSCALL_ARG2(frame), 1); break;
        case SYS_SETFSUID: SYSCALL_RET(frame) = (uint64_t)cred_set_fsuid((uint32_t)SYSCALL_ARG0(frame)); break;
        case SYS_SETFSGID: SYSCALL_RET(frame) = (uint64_t)cred_set_fsgid((uint32_t)SYSCALL_ARG0(frame)); break;
        case SYS_GETGROUPS:
            SYSCALL_RET(frame) = (uint64_t)sys_getgroups((int64_t)SYSCALL_ARG0(frame), SYSCALL_ARG1(frame));
            break;
        case SYS_SETGROUPS:
            SYSCALL_RET(frame) = (uint64_t)sys_setgroups((int64_t)SYSCALL_ARG0(frame), SYSCALL_ARG1(frame));
            break;
        case SYS_SETPGID: SYSCALL_RET(frame) = (uint64_t)process_setpgid((int64_t)SYSCALL_ARG0(frame), (int64_t)SYSCALL_ARG1(frame)); break;
        case SYS_GETPPID: SYSCALL_RET(frame) = process_current_ppid(); break;
        case SYS_GETPGRP: SYSCALL_RET(frame) = process_current() ? process_current()->pgid : 0; break;
        case SYS_SETSID: SYSCALL_RET(frame) = (uint64_t)process_setsid(); break;
        case SYS_GETPGID: {
            process_table_lock();
            struct process *target = SYSCALL_ARG0(frame) ? process_find(SYSCALL_ARG0(frame)) : process_current();
            SYSCALL_RET(frame) = target ? target->pgid : (uint64_t)-(int64_t)ESRCH;
            process_table_unlock();
            break;
        }
        case SYS_GETSID: {
            process_table_lock();
            struct process *target = SYSCALL_ARG0(frame) ? process_find(SYSCALL_ARG0(frame)) : process_current();
            SYSCALL_RET(frame) = target ? target->sid : (uint64_t)-(int64_t)ESRCH;
            process_table_unlock();
            break;
        }
        case SYS_CAPGET: SYSCALL_RET(frame) = (uint64_t)sys_capget(SYSCALL_ARG0(frame), SYSCALL_ARG1(frame)); break;
        case SYS_CAPSET: SYSCALL_RET(frame) = (uint64_t)sys_capset(SYSCALL_ARG0(frame), SYSCALL_ARG1(frame)); break;
        case SYS_SIGALTSTACK: SYSCALL_RET(frame) = (uint64_t)sys_sigaltstack(frame, SYSCALL_ARG0(frame), SYSCALL_ARG1(frame)); break;
        case SYS_PRCTL: SYSCALL_RET(frame) = (uint64_t)sys_prctl((int)SYSCALL_ARG0(frame), SYSCALL_ARG1(frame), SYSCALL_ARG2(frame), SYSCALL_ARG3(frame), SYSCALL_ARG4(frame)); break;
        case SYS_ARCH_PRCTL: SYSCALL_RET(frame) = (uint64_t)sys_arch_prctl((int)SYSCALL_ARG0(frame), SYSCALL_ARG1(frame)); break;
        case SYS_FUTEX: {
            int operation = (int)SYSCALL_ARG1(frame);
            int command = operation & FUTEX_CMD_MASK;
            int bitset_form = command == FUTEX_WAIT_BITSET || command == FUTEX_WAKE_BITSET;

            uint32_t bitset = bitset_form ? (uint32_t)SYSCALL_ARG5(frame) : FUTEX_BITSET_MATCH_ANY;
            if (!bitset) {
                SYSCALL_RET(frame) = (uint64_t)-(int64_t)EINVAL;
                break;
            }

            int shared = !(operation & FUTEX_PRIVATE_FLAG);
            if (command == FUTEX_WAKE || command == FUTEX_WAKE_BITSET) {
                SYSCALL_RET(frame) = (uint64_t)process_futex_wake(SYSCALL_ARG0(frame), (int)SYSCALL_ARG2(frame),
                                                          bitset, shared);
            } else if (command == FUTEX_WAIT || command == FUTEX_WAIT_BITSET) {
                int64_t timeout_ns = -1;
                if (SYSCALL_ARG3(frame)) {
                    struct linux_timespec timeout;
                    if (copy_from_user(&timeout, SYSCALL_ARG3(frame), sizeof(timeout)) != 0) {
                        SYSCALL_RET(frame) = (uint64_t)-(int64_t)EFAULT;
                        break;
                    }
                    if (timeout.tv_sec < 0 || timeout.tv_nsec < 0 || timeout.tv_nsec >= 1000000000LL) {
                        SYSCALL_RET(frame) = (uint64_t)-(int64_t)EINVAL;
                        break;
                    }
                    timeout_ns = timeout.tv_sec > (INT64_MAX - timeout.tv_nsec) / 1000000000LL ?
                        INT64_MAX : timeout.tv_sec * 1000000000LL + timeout.tv_nsec;

                    if (command == FUTEX_WAIT_BITSET) {
                        uint64_t now = (operation & FUTEX_CLOCK_REALTIME)
                            ? time_realtime_ns() : time_uptime_ns();
                        timeout_ns = (uint64_t)timeout_ns > now
                            ? (int64_t)((uint64_t)timeout_ns - now) : 0;
                    }
                }
                struct process *futex_caller = process_current();
                int64_t result = process_futex_wait(frame, SYSCALL_ARG0(frame), (uint32_t)SYSCALL_ARG2(frame),
                                                    timeout_ns, bitset, shared);
                if (process_current() == futex_caller) SYSCALL_RET(frame) = (uint64_t)result;
            } else {
                SYSCALL_RET(frame) = (uint64_t)-(int64_t)ENOSYS;
            }
            break;
        }
        case SYS_SET_TID_ADDRESS: {
            struct process *process = process_current();
            if (process) process->clear_child_tid_user = SYSCALL_ARG0(frame);
            SYSCALL_RET(frame) = process_current_pid();
            break;
        }
        case SYS_CLOCK_GETTIME: SYSCALL_RET(frame) = (uint64_t)sys_clock_gettime((int)SYSCALL_ARG0(frame), SYSCALL_ARG1(frame)); break;
        case SYS_CLOCK_GETRES: {
            struct linux_timespec value = {0, 1000000};
            SYSCALL_RET(frame) = SYSCALL_ARG1(frame) && copy_to_user(SYSCALL_ARG1(frame), &value, sizeof(value)) != 0 ? (uint64_t)-(int64_t)EFAULT : 0;
            break;
        }
        case SYS_CLOCK_NANOSLEEP: {
            if ((int)SYSCALL_ARG0(frame) != 0 && (int)SYSCALL_ARG0(frame) != 1 && (int)SYSCALL_ARG0(frame) != 7) {
                SYSCALL_RET(frame) = (uint64_t)-(int64_t)EINVAL;
                break;
            }
            if (SYSCALL_ARG1(frame) & ~1ULL) {
                SYSCALL_RET(frame) = (uint64_t)-(int64_t)EINVAL;
                break;
            }
            struct linux_timespec request;
            if (copy_from_user(&request, SYSCALL_ARG2(frame), sizeof(request)) != 0 ||
                request.tv_sec < 0 || request.tv_nsec < 0 ||
                request.tv_nsec >= 1000000000LL) {
                SYSCALL_RET(frame) = (uint64_t)-(int64_t)EINVAL;
                break;
            }
            uint64_t requested = (uint64_t)request.tv_sec * 1000000000ULL +
                                 (uint64_t)request.tv_nsec;
            int64_t duration;
            if (SYSCALL_ARG1(frame) & 1ULL) {
                uint64_t now = (int)SYSCALL_ARG0(frame) == 0 ? time_realtime_ns() : time_uptime_ns();
                duration = requested <= now ? 0 :
                    (requested - now > (uint64_t)INT64_MAX ? INT64_MAX :
                     (int64_t)(requested - now));
            } else {
                duration = requested > (uint64_t)INT64_MAX ? INT64_MAX : (int64_t)requested;
            }
            if (duration == 0) SYSCALL_RET(frame) = 0;
            else {
                io_watch_begin(process_current());
                if (!retry_io_wait(frame, SYS_CLOCK_NANOSLEEP, duration)) SYSCALL_RET(frame) = 0;
            }
            break;
        }
        case SYS_INOTIFY_INIT: SYSCALL_RET(frame) = (uint64_t)sys_inotify_init(0); break;
        case SYS_INOTIFY_ADD_WATCH:
            SYSCALL_RET(frame) = (uint64_t)sys_inotify_add_watch((int)SYSCALL_ARG0(frame), SYSCALL_ARG1(frame),
                                                         (uint32_t)SYSCALL_ARG2(frame));
            break;
        case SYS_INOTIFY_RM_WATCH:
            SYSCALL_RET(frame) = (uint64_t)sys_inotify_rm_watch((int)SYSCALL_ARG0(frame), (int)SYSCALL_ARG1(frame));
            break;
        case SYS_TIMERFD_CREATE:
            SYSCALL_RET(frame) = (uint64_t)sys_timerfd_create((int)SYSCALL_ARG0(frame), (int)SYSCALL_ARG1(frame));
            break;
        case SYS_EVENTFD:
            SYSCALL_RET(frame) = (uint64_t)sys_eventfd((uint32_t)SYSCALL_ARG0(frame), 0, 1);
            break;
        case SYS_TIMERFD_SETTIME:
            SYSCALL_RET(frame) = (uint64_t)sys_timerfd_settime((int)SYSCALL_ARG0(frame),
                (int)SYSCALL_ARG1(frame), SYSCALL_ARG2(frame), SYSCALL_ARG3(frame));
            break;
        case SYS_TIMERFD_GETTIME:
            SYSCALL_RET(frame) = (uint64_t)sys_timerfd_gettime((int)SYSCALL_ARG0(frame), SYSCALL_ARG1(frame));
            break;
        case SYS_EVENTFD2:
            SYSCALL_RET(frame) = (uint64_t)sys_eventfd((uint32_t)SYSCALL_ARG0(frame),
                                               (int)SYSCALL_ARG1(frame), 0);
            break;
        case SYS_EPOLL_CREATE1:
            SYSCALL_RET(frame) = (uint64_t)sys_epoll_create((int)SYSCALL_ARG0(frame));
            break;
        case SYS_INOTIFY_INIT1:
            SYSCALL_RET(frame) = (uint64_t)sys_inotify_init((int)SYSCALL_ARG0(frame));
            break;
        case SYS_OPENAT: SYSCALL_RET(frame) = (uint64_t)open_at((int)SYSCALL_ARG0(frame), SYSCALL_ARG1(frame), SYSCALL_OPEN_FLAGS_IN(SYSCALL_ARG2(frame)), SYSCALL_ARG3(frame)); break;
        case SYS_MKNOD:
            SYSCALL_RET(frame) = (uint64_t)sys_mknodat(AT_FDCWD, SYSCALL_ARG0(frame),
                                               (uint32_t)SYSCALL_ARG1(frame), SYSCALL_ARG2(frame));
            break;
        case SYS_MKNODAT:
            SYSCALL_RET(frame) = (uint64_t)sys_mknodat((int)SYSCALL_ARG0(frame), SYSCALL_ARG1(frame),
                                               (uint32_t)SYSCALL_ARG2(frame), SYSCALL_ARG3(frame));
            break;
        case SYS_MKDIRAT: SYSCALL_RET(frame) = (uint64_t)sys_mkdir_at((int)SYSCALL_ARG0(frame), SYSCALL_ARG1(frame), SYSCALL_ARG2(frame)); break;
        case SYS_NEWFSTATAT:
            if (SYSCALL_ARG3(frame) & ~(AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH | AT_NO_AUTOMOUNT))
                SYSCALL_RET(frame) = (uint64_t)-(int64_t)EINVAL;
            else {
                char first = 0;
                if (copy_from_user(&first, SYSCALL_ARG1(frame), 1) != 0) SYSCALL_RET(frame) = (uint64_t)-(int64_t)EFAULT;
                else if (!first && (SYSCALL_ARG3(frame) & AT_EMPTY_PATH) && (int)SYSCALL_ARG0(frame) >= 0)
                    SYSCALL_RET(frame) = (uint64_t)sys_fstat((int)SYSCALL_ARG0(frame), SYSCALL_ARG2(frame));
                else SYSCALL_RET(frame) = (uint64_t)stat_path((int)SYSCALL_ARG0(frame), SYSCALL_ARG1(frame), SYSCALL_ARG2(frame),
                                                      (SYSCALL_ARG3(frame) & AT_SYMLINK_NOFOLLOW) == 0);
            }
            break;
        case SYS_UNLINKAT: SYSCALL_RET(frame) = (uint64_t)sys_unlink_at((int)SYSCALL_ARG0(frame), SYSCALL_ARG1(frame), (int)SYSCALL_ARG2(frame)); break;
        case SYS_RENAMEAT: SYSCALL_RET(frame) = (uint64_t)sys_rename_at((int)SYSCALL_ARG0(frame), SYSCALL_ARG1(frame), (int)SYSCALL_ARG2(frame), SYSCALL_ARG3(frame), 0); break;
        case SYS_SYMLINK: SYSCALL_RET(frame) = (uint64_t)sys_symlink_at(SYSCALL_ARG0(frame), AT_FDCWD, SYSCALL_ARG1(frame)); break;
        case SYS_SYMLINKAT: SYSCALL_RET(frame) = (uint64_t)sys_symlink_at(SYSCALL_ARG0(frame), (int)SYSCALL_ARG1(frame), SYSCALL_ARG2(frame)); break;
        case SYS_MOUNT: SYSCALL_RET(frame) = (uint64_t)sys_mount(SYSCALL_ARG0(frame), SYSCALL_ARG1(frame), SYSCALL_ARG2(frame), SYSCALL_ARG3(frame), SYSCALL_ARG4(frame)); break;
        case SYS_UMOUNT2: SYSCALL_RET(frame) = (uint64_t)sys_umount2(SYSCALL_ARG0(frame), (int)SYSCALL_ARG1(frame)); break;
        case SYS_LINK: SYSCALL_RET(frame) = (uint64_t)sys_link_at(AT_FDCWD, SYSCALL_ARG0(frame), AT_FDCWD, SYSCALL_ARG1(frame), 0); break;
        case SYS_LINKAT: SYSCALL_RET(frame) = (uint64_t)sys_link_at((int)SYSCALL_ARG0(frame), SYSCALL_ARG1(frame), (int)SYSCALL_ARG2(frame), SYSCALL_ARG3(frame), (int)SYSCALL_ARG4(frame)); break;
        case SYS_READLINKAT: SYSCALL_RET(frame) = (uint64_t)sys_readlink_at((int)SYSCALL_ARG0(frame), SYSCALL_ARG1(frame), SYSCALL_ARG2(frame), (size_t)SYSCALL_ARG3(frame)); break;
        case SYS_FCHMODAT: SYSCALL_RET(frame) = (uint64_t)sys_chmod_at((int)SYSCALL_ARG0(frame), SYSCALL_ARG1(frame), (uint32_t)SYSCALL_ARG2(frame), 0); break;
        case SYS_UTIMENSAT:
            SYSCALL_RET(frame) = (uint64_t)sys_utimens_at((int)SYSCALL_ARG0(frame), SYSCALL_ARG1(frame),
                                                  SYSCALL_ARG2(frame), (int)SYSCALL_ARG3(frame));
            break;
        case SYS_SET_ROBUST_LIST: SYSCALL_RET(frame) = (uint64_t)sys_set_robust_list(SYSCALL_ARG0(frame), (size_t)SYSCALL_ARG1(frame)); break;
        case SYS_GET_ROBUST_LIST: SYSCALL_RET(frame) = (uint64_t)sys_get_robust_list((int)SYSCALL_ARG0(frame), SYSCALL_ARG1(frame), SYSCALL_ARG2(frame)); break;
        case SYS_ACCEPT4:
            accept_or_block(frame, SYS_ACCEPT4, (int)SYSCALL_ARG0(frame), SYSCALL_ARG1(frame), SYSCALL_ARG2(frame), (int)SYSCALL_ARG3(frame));
            break;
        case SYS_PIPE2: SYSCALL_RET(frame) = (uint64_t)sys_pipe(SYSCALL_ARG0(frame), (int)SYSCALL_ARG1(frame)); break;
        case SYS_PRLIMIT64: SYSCALL_RET(frame) = (uint64_t)sys_prlimit(SYSCALL_ARG0(frame), SYSCALL_ARG1(frame), SYSCALL_ARG2(frame), SYSCALL_ARG3(frame)); break;
        case SYS_RENAMEAT2: SYSCALL_RET(frame) = (uint64_t)sys_rename_at((int)SYSCALL_ARG0(frame), SYSCALL_ARG1(frame), (int)SYSCALL_ARG2(frame), SYSCALL_ARG3(frame), (unsigned)SYSCALL_ARG4(frame)); break;
        case SYS_GETRANDOM: SYSCALL_RET(frame) = (uint64_t)sys_getrandom(SYSCALL_ARG0(frame), (size_t)SYSCALL_ARG1(frame), (unsigned)SYSCALL_ARG2(frame)); break;
        case SYS_GETDENTS64: SYSCALL_RET(frame) = (uint64_t)sys_getdents64((int)SYSCALL_ARG0(frame), SYSCALL_ARG1(frame), (size_t)SYSCALL_ARG2(frame)); break;
        case SYS_STATFS: SYSCALL_RET(frame) = (uint64_t)sys_statfs(SYSCALL_ARG0(frame), SYSCALL_ARG1(frame)); break;
        case SYS_FSTATFS: SYSCALL_RET(frame) = (uint64_t)sys_fstatfs((int)SYSCALL_ARG0(frame), SYSCALL_ARG1(frame)); break;
        case SYS_STATX:
            SYSCALL_RET(frame) = (uint64_t)sys_statx((int)SYSCALL_ARG0(frame), SYSCALL_ARG1(frame), (int)SYSCALL_ARG2(frame),
                                             (uint32_t)SYSCALL_ARG3(frame), SYSCALL_ARG4(frame));
            break;
        case SYS_RSEQ: SYSCALL_RET(frame) = (uint64_t)-(int64_t)ENOSYS; break;
        case SYS_FACCESSAT2:
            SYSCALL_RET(frame) = (uint64_t)sys_faccess_at((int)SYSCALL_ARG0(frame), SYSCALL_ARG1(frame),
                                                  (int)SYSCALL_ARG2(frame), (int)SYSCALL_ARG3(frame));
            break;
        case SYS_CLOSE_RANGE: {
            struct process *process = process_current();
            uint64_t first = SYSCALL_ARG0(frame), last = SYSCALL_ARG1(frame), flags = SYSCALL_ARG2(frame);
            if (!process || first >= (uint64_t)PROCESS_FD_CAPACITY(process)) SYSCALL_RET(frame) = 0;
            else {
                if (last >= (uint64_t)PROCESS_FD_CAPACITY(process))
                    last = (uint64_t)PROCESS_FD_CAPACITY(process) - 1;
                if (flags & CLOSE_RANGE_CLOEXEC) {
                    for (uint64_t fd = first; fd <= last; fd++)
                        (void)process_set_fd_flags(process, (int)fd, PROCESS_FD_CLOEXEC);
                } else {
                    for (uint64_t fd = first; fd <= last; fd++)
                        (void)process_close_fd(process, (int)fd);
                }
                SYSCALL_RET(frame) = 0;
            }
            break;
        }
        default:

            KDEBUG("syscall: ENOSYS pid=%u nr=%u\n",
                    (unsigned)process_current_pid(), (unsigned)syscall_number);
            SYSCALL_RET(frame) = (uint64_t)-(int64_t)ENOSYS;
            break;
    }

    if (caller && caller == process_current())
        note_would_block(frame, syscall_number, first_argument);
    if (!skip_signal_delivery) process_prepare_user_return(frame);
}

#if defined(__x86_64__)
_Static_assert(sizeof(struct syscall_frame) == 144, "syscall_entry.S assumes 144");
#endif

static int syscall_writes_data(uint64_t number) {
    switch (number) {
        case SYS_WRITE:
        case SYS_WRITEV:
        case SYS_PWRITE64:
        case SYS_PWRITEV:
            return 1;
        default:
            return 0;
    }
}

#define VERBOSE_SYSCALL_LIMIT 24U

void syscall_dispatch(struct syscall_frame *frame) {
    uint64_t syscall_number = SYSCALL_NR(frame);
    struct process *caller = process_current();
    defer_kernel_enter();
    kernel_overlap_sample();
    process_note_syscall_entry();

    if (boot_verbose()) {
        static unsigned traced;
        if (traced < VERBOSE_SYSCALL_LIMIT) {
            traced++;
            kprintf("syscall: %u from pid %d\n", (unsigned)SYSCALL_NR(frame),
                    (int)process_current_pid());
        }
    }

    syscall_run(frame);
    syscall_release_pins_of(caller);
    if (syscall_writes_data(syscall_number) && process_current() == caller) vfs_balance_dirty();
    struct syscall_frame *resumed = frame;
    uint64_t stack_top = cpu_current()->kernel_rsp;
    if (stack_top) {
        resumed = (struct syscall_frame *)(stack_top - sizeof(*frame));
        if (resumed != frame) *resumed = *frame;
    }
    if ((int64_t)SYSCALL_RET(resumed) == -(int64_t)ENOSYS)
        abi_gaps_note(syscall_number);
}
