#ifndef _SYS_UCONTEXT_H
#define _SYS_UCONTEXT_H

/* Minimal kernel-side stub for TCC's unix path (x86_64 build in CFOS).
 * tcc_run() is never executed in the kernel build, and the fbreak
 * function rt_getcontext() is never called, but the compiler still has
 * to resolve the references in tccrun.c. */

#define REG_R8  0
#define REG_R9  1
#define REG_R10 2
#define REG_R11 3
#define REG_R12 4
#define REG_R13 5
#define REG_R14 6
#define REG_R15 7
#define REG_RDI 8
#define REG_RSI 9
#define REG_RBP 10
#define REG_RBX 11
#define REG_RDX 12
#define REG_RAX 13
#define REG_RCX 14
#define REG_RSP 15
#define REG_RIP 16
#define REG_EFL 17

typedef struct {
    long gregs[24];
} kernel_stub_mcontext;

typedef struct __ucontext {
    unsigned long        uc_flags;
    struct __ucontext   *uc_link;
    kernel_stub_mcontext uc_mcontext;
    long __pad[8];
} ucontext_t;

#endif /* _SYS_UCONTEXT_H */
