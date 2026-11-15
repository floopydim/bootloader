#ifndef _SIGNAL_H
#define _SIGNAL_H

/* Minimal, kernel-side stub for TCC (tccrun.c references these on the
 * unix path). In the CFOS kernel build these functions are never called;
 * matching stubs live in tcc_libc.c. */

#ifndef SIG_BLOCK
# define SIG_BLOCK  0
# define SIG_UNBLOCK 1
# define SIG_SETMASK 2
#endif

#ifndef SIGFPE
# define SIGFPE  8
# define SIGILL  4
# define SIGSEGV 11
# define SIGABRT 6
# define SIGBUS  7
# define SIGINT  2
# define SIGTRAP 5
#endif

#ifndef FPE_INTDIV
# define FPE_INTDIV  1
# define FPE_INTOVF  2
# define FPE_FLTDIV  3
# define FPE_FLTOVF  4
# define FPE_FLTUND  5
# define FPE_FLTRES  6
# define FPE_FLTINV  7
# define FPE_FLTSUB  8
#endif

#ifndef SA_NOCLDSTOP
# define SA_NOCLDSTOP 1
# define SA_NOCLDWAIT 2
# define SA_SIGINFO   4
# define SA_ONSTACK   0x08000000
# define SA_RESTART   0x10000000
# define SA_NODEFER   0x40000000
# define SA_RESETHAND 0x80000000
#endif

#if !defined(SIG_IGN) && !defined(SIG_DFL)
# define SIG_IGN ((void*)1)
# define SIG_DFL ((void*)0)
#endif

typedef struct { unsigned long __val[8]; } stub_sigset_t;
typedef stub_sigset_t sigset_t;

typedef struct {
    int si_signo;
    int si_errno;
    int si_code;
    union {
        int _pad[28];
        struct { int _pid, _uid; } _kill;
        struct { int _tid; } _thread;
    } _sifields;
} siginfo_t;

struct sigaction {
    void (*sa_handler)(int);
    void (*sa_sigaction)(int, siginfo_t *, void *);
    sigset_t sa_mask;
    int      sa_flags;
    void    (*sa_restorer)(void);
};

#ifdef __cplusplus
extern "C" {
#endif

int sigemptyset(sigset_t *set);
int sigaddset(sigset_t *set, int signum);
int sigprocmask(int how, const sigset_t *set, sigset_t *oldset);
int sigaction(int signum, const struct sigaction *act, struct sigaction *oldact);

#ifdef __cplusplus
}
#endif

#endif /* _SIGNAL_H */
