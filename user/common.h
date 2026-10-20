#ifndef USER_COMMON_H
#define USER_COMMON_H

typedef unsigned long  u64;
typedef long           i64;
typedef unsigned int   u32;
typedef unsigned char  u8;

/* System call numbers (must match kernel/syscall.h). */
#define SYS_EXIT    0
#define SYS_READ    1
#define SYS_WRITE   2
#define SYS_GETPID  5
#define SYS_TIME    12

/*
 * Syscall gate: INT 0x80.
 * Inputs:  RAX = number, RDI/RSI/RDX = args.
 * Output:  RAX = result (-1 on error).
 */
static inline u64 sys3(u64 num, u64 a, u64 b, u64 c) {
    u64 ret;
    __asm__ __volatile__("int $0x80"
                         : "=a"(ret)
                         : "a"(num), "D"(a), "S"(b), "d"(c)
                         : "memory");
    return ret;
}

static inline u64 sys0(u64 num) {
    return sys3(num, 0, 0, 0);
}

static inline void write_str(const char *s) {
    u64 n = 0;
    while (s[n]) n++;
    sys3(SYS_WRITE, 1, (u64)s, n);
}

#endif
