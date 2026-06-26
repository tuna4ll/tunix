#ifndef TUNIX_SIGNAL_H
#define TUNIX_SIGNAL_H

#include <stdint.h>

#define TUNIX_NSIG 32
#define SIG_DFL 0ULL
#define SIG_IGN 1ULL

#define SIGHUP   1
#define SIGINT   2
#define SIGQUIT  3
#define SIGKILL  9
#define SIGUSR1 10
#define SIGSEGV 11
#define SIGPIPE 13
#define SIGTERM 15
#define SIGCHLD 17
#define SIGCONT 18
#define SIGSTOP 19
#define SIGTSTP 20

#define SIG_BLOCK   0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2

struct tunix_sigaction {
    uint64_t handler;
    uint64_t flags;
    uint64_t restorer;
    uint64_t mask;
};

#endif
