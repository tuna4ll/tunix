#ifndef TUNIX_SIGNAL_H
#define TUNIX_SIGNAL_H

#include <stdint.h>

#define TUNIX_NSIG 64
#define SIG_DFL 0ULL
#define SIG_IGN 1ULL

#define SIGHUP   1
#define SIGINT   2
#define SIGQUIT  3
#define SIGILL   4
#define SIGBUS   7
#define SIGFPE   8
#define SIGKILL  9
#define SIGUSR1 10
#define SIGSEGV 11
#define SIGPIPE 13
#define SIGALRM 14
#define SIGTERM 15
#define SIGCHLD 17
#define SIGCONT 18
#define SIGSTOP 19
#define SIGTSTP 20
#define SIGTTIN 21
#define SIGTTOU 22

#define SIG_BLOCK   0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2

#define SA_SIGINFO 0x00000004ULL
#define SA_ONSTACK 0x08000000ULL
#define SA_RESTART 0x10000000ULL

#define SI_USER   0
#define SI_KERNEL 0x80

#define SIGNAL_SIGINFO_SIZE 128
#define SIGNAL_CONTEXT_SIZE 1024

#define UCONTEXT_FLAGS_OFFSET 0
#define UCONTEXT_LINK_OFFSET 8
#define UCONTEXT_STACK_OFFSET 16
#define UCONTEXT_MCONTEXT_OFFSET 40
#define UCONTEXT_SIGMASK_OFFSET 296

#define MCONTEXT_R8 0
#define MCONTEXT_R9 1
#define MCONTEXT_R10 2
#define MCONTEXT_R11 3
#define MCONTEXT_R12 4
#define MCONTEXT_R13 5
#define MCONTEXT_R14 6
#define MCONTEXT_R15 7
#define MCONTEXT_RDI 8
#define MCONTEXT_RSI 9
#define MCONTEXT_RBP 10
#define MCONTEXT_RBX 11
#define MCONTEXT_RDX 12
#define MCONTEXT_RAX 13
#define MCONTEXT_RCX 14
#define MCONTEXT_RSP 15
#define MCONTEXT_RIP 16
#define MCONTEXT_EFLAGS 17
#define MCONTEXT_CSGSFS 18
#define MCONTEXT_ERR 19
#define MCONTEXT_TRAPNO 20
#define MCONTEXT_OLDMASK 21
#define MCONTEXT_CR2 22
#define MCONTEXT_REGISTERS 23

#define SS_ONSTACK 1
#define SS_DISABLE 2
#define MINSIGSTKSZ 2048ULL

struct tunix_sigaction {
    uint64_t handler;
    uint64_t flags;
    uint64_t restorer;
    uint64_t mask;
};

#endif
