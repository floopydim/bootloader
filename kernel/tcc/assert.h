#ifndef _ASSERT_H
#define _ASSERT_H

#include <stddef.h>

/* Kernel-side stub for TCC's assert() usages (x86_64 codegen, etc.).
 * The real handler lives in tcc_libc.c and panics. */

#ifdef __cplusplus
extern "C" {
#endif

void __assert_fail(const char *assertion, const char *file,
                   unsigned int line, const char *function);

#ifdef __cplusplus
}
#endif

#ifdef NDEBUG
# define assert(test) ((void)0)
#else
# define assert(test) ((test) ? (void)0 : __assert_fail(#test, __FILE__, __LINE__, __func__))
#endif

#endif /* _ASSERT_H */
