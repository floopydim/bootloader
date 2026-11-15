#include "console.h"
#include "kheap.h"
#include "partition.h"
#include <stddef.h>
#include <stdint.h>

typedef long ssize_t;

#include "tcc/config.h"
#include "tcc/libtcc.h"
#include "tcc/tcc.h"          /* internal TCCState fields (has_text_addr etc.) */
#include "tcc/stdheaders.h"   /* FILE + stdio shim prototypes */
#include "cc3_rtobj.h"

/*
 * ============ cc3: in-kernel TCC -> ring 3 ELF ============
 *
 * The kernel TCC is compiled with CONFIG_TCC_ASM=0 (no inline asm) and
 * OUTPUT_MEMORY is how the legacy `cc` command uses it (compile to kernel
 * memory, run as ring 0 script).  This module makes TCC produce a full
 * ring 3 ELF executable instead:
 *
 *   1. a fresh TCCState is configured for TCC_OUTPUT_EXE with the text
 *      base at USER_BASE (0x8000000000), matching kernel/userproc.c;
 *   2. the ring 3 runtime object (build/ccrt.o from user/tcc3rt.asm,
 *      embedded as cc3_rtobj[]) is linked through the RAM-file shim so
 *      TCC merges it with its own .text: syscall wrappers get real
 *      machine code (TCC has no inline asm in this build);
 *   3. the user's source is compiled with a prelude that declares the
 *      runtime API;
 *   4. tcc_output_file() runs the full EXE link (tcc_add_runtime +
 *      relocate_sections, see tccelf.c) and writes the ELF file through
 *      the RAM-file shim (tcc_write_elf_file -> open/fdopen/fwrite/
 *      fputc/fclose all interceptable, see tcc_libc.c);
 *   5. the captured buffer is returned; the shell writes it to a FAT32
 *      file, `run` then loads it through kernel/userproc.c.
 *
 * The file-visible layout makes one PT_LOAD (r-x) covering the ELF
 * headers + .text and one (rw) with .data/.bss; userproc.c maps each
 * PT_LOAD at its own pages and the entry lands inside the image.
 */

/* ---------------------------------------------------------------- extra
 * kernel heap based realloc for TCC (same policy as tcc_wrap.c)
 * ---------------------------------------------------------------------- */

static void *cc3_realloc_func(void *ptr, unsigned long size) {
    if (size == 0) {
        if (ptr) kfree(ptr);
        return NULL;
    }
    if (!ptr) return kmalloc(size);
    return krealloc(ptr, (size_t)size);
}

/* ------------------------------------------------------------ RAM files
 * The stdio shim in tcc_libc.c routes three paths here:
 *   "@ccrt.o"  — read-only stream over the embedded ring 3 runtime .o
 *   "@out.elf" — write-only capture of the ELF image TCC produces
 */

#define RAM_FD_RT   128
#define RAM_FD_OUT  129
#define RAM_POS_RT  g_cc_rt_pos

typedef struct {
    unsigned char *base;
    size_t cap;   /* allocated capacity */
    size_t len;   /* used bytes */
    size_t pos;   /* read/write cursor */
} capture_t;

static capture_t     g_cc_out;
static FILE          g_cc_out_file; /* FILE defined in tcc/stdheaders.h */
static unsigned long g_cc_rt_pos;

void capture_reset(capture_t *c) {
    if (c->base) {
        kfree(c->base);
        c->base = NULL;
    }
    c->cap  = 0;
    c->len  = 0;
    c->pos  = 0;
}

int capture_write(capture_t *c, const void *buf, size_t n) {
    if (c->len + n > c->cap) {
        size_t newcap = c->cap ? c->cap * 2 : 65536;
        while (newcap < c->len + n) newcap *= 2;
        unsigned char *nb = krealloc(c->base, newcap);
        if (!nb) return -1;
        c->base = nb;
        c->cap = newcap;
    }
    unsigned char *d = c->base + c->len;
    const unsigned char *s = buf;
    for (size_t i = 0; i < n; i++) d[i] = s[i];
    c->len += n;
    return 0;
}

int capture_read(capture_t *c, void *buf, size_t n) {
    if (c->pos >= c->len) return 0;
    if (c->pos + n > c->len) n = c->len - c->pos;
    unsigned char *d = buf;
    unsigned char *s = c->base + c->pos;
    for (size_t i = 0; i < n; i++) d[i] = s[i];
    c->pos += n;
    return n;
}

/* Identify a RAM file path. Returns fd or -1. */
static int cc_ram_identify(const char *path) {
    if (path[0] == '@') {
        if (path[1] == 'c' && path[2] == 'c' &&
            path[3] == 'r' && path[4] == 't' && path[5] == '.' &&
            path[6] == 'o' && path[7] == '\0') return RAM_FD_RT;
        if (path[1] == 'o' && path[2] == 'u' && path[3] == 't' &&
            path[4] == '.' && path[5] == 'e' && path[6] == 'l' &&
            path[7] == 'f' && path[8] == '\0') return RAM_FD_OUT;
    }
    return -1;
}

int cc_ram_open(const char *path) {
    int fd = cc_ram_identify(path);
    if (fd < 0) return -1;
    if (fd == RAM_FD_RT) g_cc_rt_pos = 0;
    return fd;
}

int cc_ram_close(int fd) {
    if (fd != RAM_FD_RT && fd != RAM_FD_OUT) return -1;
    return 0;
}

ssize_t cc_ram_read(int fd, void *buf, size_t count) {
    if (fd != RAM_FD_RT) return -1;
    size_t remain;
    if (g_cc_rt_pos >= CC3_RTOBJ_SIZE) return 0;
    remain = CC3_RTOBJ_SIZE - g_cc_rt_pos;
    if (remain > count) remain = count;
    const unsigned char *s = cc3_rtobj + g_cc_rt_pos;
    unsigned char *d = buf;
    for (size_t i = 0; i < remain; i++) d[i] = s[i];
    g_cc_rt_pos += (unsigned long)remain;
    return (ssize_t)remain;
}

ssize_t cc_ram_lseek(int fd, long off, int whence) {
    unsigned long *pos = (fd == RAM_FD_RT) ? &RAM_POS_RT : &g_cc_out.pos;
    unsigned long lim;
    switch (whence) {
        case 0: if ((unsigned long)off > 0xFFFFFFFFULL) return -1; *pos = (unsigned long)off; break;
        case 1: *pos += (unsigned long)off; break;
        case 2: {
            lim = (fd == RAM_FD_RT) ? (unsigned long)CC3_RTOBJ_SIZE : g_cc_out.len;
            unsigned long o = (unsigned long)off < lim ? (unsigned long)off : lim;
            o = lim - o;
            *pos = o;
            break;
        }
        default: return -1;
    }
    return *pos;
}

/* Write helpers used by the stdio layer for the "@out.elf" FILE. */

int cc_out_is(void *stream) {
    return stream == (void *)&g_cc_out_file;
}

FILE *cc_out_fdopen(int fd, const char *mode) {
    if (fd != RAM_FD_OUT || !mode || mode[0] != 'w') return NULL;
    g_cc_out_file._fd = RAM_FD_OUT;
    g_cc_out_file._eof = 0;
    g_cc_out_file._err = 0;
    g_cc_out_file._base = NULL;
    g_cc_out_file._len = 0;
    g_cc_out_file._pos = 0;
    return &g_cc_out_file;
}

int cc_out_write(const void *buf, size_t bytes) {
    size_t n = bytes;
    while (n) {
        const size_t chunk = n > 1 << 30 ? (size_t)1 << 30 : n;
        if (capture_write(&g_cc_out, buf, chunk) < 0) return -1;
        n -= chunk;
    }
    return 0;
}

FILE *cc_out_stream(void) { return &g_cc_out_file; }

size_t cc_out_file_len(void) { return g_cc_out.len; }
unsigned char *cc_out_file_buf(void) { return g_cc_out.base; }

/* -------------------------------------------------------------- prelude
 * Declared before the user's program source so that calls resolve to
 * runtime functions provided by cc3rt.o (linked via "@ccrt.o").
 * ---------------------------------------------------------------------- */

static const char CC3_PRELUDE_SRC[] =
    "typedef unsigned long ccu64;\n"
    "typedef long          cci64;\n"
    "\n"
    "extern unsigned long s_write(const void *buf, unsigned long n);\n"
    "extern unsigned long s_read(void *buf, unsigned long n);\n"
    "extern void  s_exit(int code);\n"
    "extern void  s_puts(const char *s);\n"
    "extern void  s_nl(void);\n"
    "extern int   getchar(void);\n"
    "extern unsigned long getpid(void);\n"
    "extern unsigned long uptime(void);\n"
    "extern void  print_i64(long v);\n"
    "extern void  print_u64(unsigned long v);\n"
    "extern void  print_hex64(unsigned long v);\n"
    "extern void *memcpy(void *dst, const void *src, ccu64 n);\n"
    "extern void *memmove(void *dst, const void *src, ccu64 n);\n"
    "extern void *memset(void *dst, int c, ccu64 n);\n"
    ;

/* --------------------------------------------------------------- errors */

static void cc3_error_cb(void *opaque, const char *msg) {
    CONSOLE *con = (CONSOLE *)opaque;
    if (con) console_printf(con, "cc3: %s\n", msg);
}

/* ------------------------------------------------------------- compile
 * Compile `source` into a ring 3 ELF image held in freshly kmalloc'd
 * memory. On success returns 0 and sets *out_img / *out_size (caller must
 * kfree). On failure returns -1 and prints compiler diagnostics.
 * ---------------------------------------------------------------------- */

int tcc_kernel_compile_elf(CONSOLE *con,
                           const char *source,
                           uint8_t **out_img,
                           uint32_t *out_size)
{
    TCCState *s;
    int rc = -1;

    if (!con || !source || !out_img || !out_size) return -2;
    *out_img = NULL;
    *out_size = 0;

    tcc_set_realloc(cc3_realloc_func);

    s = tcc_new();
    if (!s) {
        console_printf(con, "cc3: cannot create compiler state\n");
        return -3;
    }
    tcc_set_error_func(s, (void *)con, cc3_error_cb);

    /* no external crt/lib paths — disk is empty anyway */
    s->nostdlib      = 1;
    s->nostdlib_paths = 1;
    s->nostdinc      = 1;

    /* link everything at USER_BASE (kernel/userproc.c user range) */
    s->has_text_addr = 1;
    s->text_addr = 0x0000008000000000ULL;

    tcc_set_output_type(s, TCC_OUTPUT_EXE);
    s->output_format = TCC_OUTPUT_FORMAT_ELF;

    /* merge the ring 3 runtime object (provides _start/syscall wrappers) */
    if (tcc_add_file(s, "@ccrt.o") != 0) {
        console_printf(con, "cc3: cannot load runtime object\n");
        goto done;
    }

    /* user program; prelude declares the runtime API so calls resolve
     * against the merged cc3rt.o symbols */
    if (tcc_compile_string(s, CC3_PRELUDE_SRC) != 0) {
        console_printf(con, "cc3: prelude failed\n");
        goto done;
    }
    if (tcc_compile_string(s, source) != 0) {
        console_printf(con, "cc3: compilation failed\n");
        goto done;
    }

    /* full EXE link happens inside tcc_output_file (tccelf.c):
     * tcc_add_runtime, resolve_common_syms, relocate_sections, then the
     * ELF image is written through the stdio shim into our capture. */
    capture_reset(&g_cc_out);
    g_cc_out_file._fd = RAM_FD_OUT;
    if (tcc_output_file(s, "@out.elf") != 0) {
        console_printf(con, "cc3: cannot emit ELF image\n");
        goto done;
    }

    if (g_cc_out.len == 0 || !g_cc_out.base) {
        console_printf(con, "cc3: compiler produced no output\n");
        goto done;
    }

    *out_img  = g_cc_out.base;
    *out_size = (uint32_t)g_cc_out.len;
    /* transfer ownership of the buffer */
    g_cc_out.base = NULL;
    g_cc_out.len  = 0;
    rc = 0;

done:
    capture_reset(&g_cc_out);
    tcc_delete(s);
    return rc;
}
