#ifndef TUNIX_GLIB_COMPAT_H
#define TUNIX_GLIB_COMPAT_H

#include <stddef.h>
#include <stdint.h>

#ifndef T_O_NONBLOCK
#define T_O_NONBLOCK 04000
#endif
#ifndef T_O_CLOEXEC
#define T_O_CLOEXEC 02000000
#endif

#ifndef TUNIX_T_TIMESPEC_DEFINED
#define TUNIX_T_TIMESPEC_DEFINED
struct t_timespec {
    int64_t tv_sec;
    int64_t tv_nsec;
};
#endif
#define T_F_DUPFD 0
#define T_F_GETFD 1
#define T_F_SETFD 2
#define T_F_GETFL 3
#define T_F_SETFL 4
#define T_F_DUPFD_CLOEXEC 1030
#define T_FD_CLOEXEC 1

#define T_EFD_SEMAPHORE 1
#define T_EFD_NONBLOCK T_O_NONBLOCK
#define T_EFD_CLOEXEC T_O_CLOEXEC

#define T_TFD_TIMER_ABSTIME 1
#define T_TFD_NONBLOCK T_O_NONBLOCK
#define T_TFD_CLOEXEC T_O_CLOEXEC

#define T_EPOLLIN 0x001U
#define T_EPOLLOUT 0x004U
#define T_EPOLLERR 0x008U
#define T_EPOLLHUP 0x010U
#define T_EPOLLRDHUP 0x2000U
#define T_EPOLLET (1U << 31)
#define T_EPOLLONESHOT (1U << 30)
#define T_EPOLL_CTL_ADD 1
#define T_EPOLL_CTL_DEL 2
#define T_EPOLL_CTL_MOD 3
#define T_EPOLL_CLOEXEC T_O_CLOEXEC

#define T_IN_MODIFY      0x00000002U
#define T_IN_ATTRIB      0x00000004U
#define T_IN_MOVED_FROM  0x00000040U
#define T_IN_MOVED_TO    0x00000080U
#define T_IN_CREATE      0x00000100U
#define T_IN_DELETE      0x00000200U
#define T_IN_DELETE_SELF 0x00000400U
#define T_IN_MOVE_SELF   0x00000800U
#define T_IN_Q_OVERFLOW  0x00004000U
#define T_IN_IGNORED     0x00008000U
#define T_IN_NONBLOCK T_O_NONBLOCK
#define T_IN_CLOEXEC T_O_CLOEXEC

#define T_SOL_SOCKET 1
#define T_SCM_RIGHTS 1
#define T_SCM_CREDENTIALS 2
#define T_SO_TYPE 3
#define T_SO_ERROR 4
#define T_SO_PASSCRED 16
#define T_SO_PEERCRED 17
#define T_SO_ACCEPTCONN 30
#define T_MSG_CTRUNC 0x08
#define T_MSG_CMSG_CLOEXEC 0x40000000

struct t_itimerspec {
    struct t_timespec interval;
    struct t_timespec value;
};

struct t_epoll_event {
    uint32_t events;
    uint64_t data;
} __attribute__((packed));

struct t_inotify_event {
    int32_t wd;
    uint32_t mask;
    uint32_t cookie;
    uint32_t length;
    char name[];
};

struct t_iovec {
    void *base;
    uint64_t length;
};

struct t_msghdr {
    void *name;
    uint32_t name_length;
    uint32_t __pad0;
    struct t_iovec *iov;
    uint64_t iov_length;
    void *control;
    uint64_t control_length;
    int32_t flags;
    uint32_t __pad1;
};

struct t_cmsghdr {
    uint64_t length;
    int32_t level;
    int32_t type;
};

struct t_ucred {
    int32_t pid;
    uint32_t uid;
    uint32_t gid;
};

#define T_CMSG_ALIGN(value) (((value) + sizeof(uint64_t) - 1U) & ~(sizeof(uint64_t) - 1U))
#define T_CMSG_SPACE(length) T_CMSG_ALIGN(sizeof(struct t_cmsghdr) + (length))
#define T_CMSG_LEN(length) (sizeof(struct t_cmsghdr) + (length))
#define T_CMSG_DATA(header) ((unsigned char *)(header) + sizeof(struct t_cmsghdr))

_Static_assert(sizeof(struct t_epoll_event) == 12, "epoll event ABI mismatch");
_Static_assert(sizeof(struct t_msghdr) == 56, "msghdr ABI mismatch");
_Static_assert(sizeof(struct t_cmsghdr) == 16, "cmsghdr ABI mismatch");

#endif
