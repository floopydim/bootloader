#ifndef _SYS_MMAN_H
#define _SYS_MMAN_H

/* Kernel-side stub for TCC (CONFIG_TCC). Only prototypes/constants that
 * tccrun.c references are provided; the kernel build never actually uses
 * them (mprotect etc. are stubbed in tcc_libc.c). */

#ifndef PROT_READ
# define PROT_READ  1
#endif
#ifndef PROT_WRITE
# define PROT_WRITE 2
#endif
#ifndef PROT_EXEC
# define PROT_EXEC  4
#endif
#ifndef PROT_NONE
# define PROT_NONE  0
#endif

#define MAP_FAILED ((void *)-1)

#ifndef MAP_SHARED
# define MAP_SHARED  0x01
#endif
#ifndef MAP_PRIVATE
# define MAP_PRIVATE 0x02
#endif
#ifndef MAP_ANONYMOUS
# define MAP_ANONYMOUS 0x20
#endif
#ifndef MAP_ANON
# define MAP_ANON MAP_ANONYMOUS
#endif

#ifndef _SC_PAGESIZE
# define _SC_PAGESIZE 30
#endif

struct mallinfo_stub { int junk; };

#ifdef __cplusplus
extern "C" {
#endif

typedef struct { int __junk; } kernel_mman_ctx;

long       sysconf(int name);
void      *mmap(void *addr, unsigned long len, int prot, int flags, int fd, long off);
int        munmap(void *addr, unsigned long len);
int        mprotect(void *addr, unsigned long len, int prot);

#ifdef __cplusplus
}
#endif

#endif /* _SYS_MMAN_H */
