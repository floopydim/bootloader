#include "shell.h"
#include "console.h"
#include "fat32.h"
#include "kheap.h"
#include "partition.h"
#include <stdint.h>

/* External functions from tcc_wrap.c */
extern int tcc_kernel_compile(const char *source, void **out_entry);
extern int tcc_kernel_run(void *entry);
extern void tcc_kernel_cleanup(void);

/* --------------------------------------------------------------------------
 * cc — compile and run a C source file from a FAT32 partition
 *
 * Usage: cc <partition> <file.c>
 *   Example: cc sd0p1 hello.c
 *   Example: cc sd0p1 /src/main.c
 * -------------------------------------------------------------------------- */

static int str_eq_cc(const char *a, const char *b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == 0 && *b == 0;
}

static int str_starts_with_cc(const char *s, const char *prefix) {
    while (*prefix) { if (*s != *prefix) return 0; s++; prefix++; }
    return 1;
}

static PARTITION_INFO *cc_find_partition(SHELL *sh, const char *name) {
    PARTITION_INFO *part = partition_find_by_name(name);
    if (!part) {
        console_printf(sh->con, "cc: partition '%s' not found\n", name);
    }
    return part;
}

static void shell_run_cc(SHELL *sh, const char *args) {
    char part_name[32];
    char file_path[128];
    uint32_t i = 0;
    uint32_t j = 0;

    /* Parse partition name */
    while (args[i] == ' ') i++;
    while (args[i] && args[i] != ' ' && j + 1 < sizeof(part_name))
        part_name[j++] = args[i++];
    part_name[j] = '\0';

    if (!part_name[0]) {
        console_printf(sh->con, "Usage: cc <partition> <file.c>\n");
        console_printf(sh->con, "  Example: cc sd0p1 hello.c\n");
        return;
    }

    /* Parse file path */
    while (args[i] == ' ') i++;
    j = 0;
    while (args[i] && j + 1 < sizeof(file_path))
        file_path[j++] = args[i++];
    file_path[j] = '\0';

    if (!file_path[0]) {
        console_printf(sh->con, "Usage: cc <partition> <file.c>\n");
        return;
    }

    /* Find and mount partition */
    PARTITION_INFO *part = cc_find_partition(sh, part_name);
    if (!part) return;

    FAT32_FS fs;
    if (!fat32_mount(part, &fs)) {
        console_printf(sh->con, "cc: cannot mount FAT32 on %s\n", part->name);
        return;
    }

    /* Read source file */
    uint8_t *src_buf = NULL;
    uint32_t src_size = 0;

    if (!fat32_read_file(part, file_path, &src_buf, &src_size)) {
        console_printf(sh->con, "cc: cannot read '%s' on %s\n", file_path, part->name);
        return;
    }

    if (src_size == 0) {
        console_printf(sh->con, "cc: '%s' is empty\n", file_path);
        kfree(src_buf);
        return;
    }

    /* Null-terminate source */
    char *source = kmalloc(src_size + 1);
    if (!source) {
        console_printf(sh->con, "cc: out of memory\n");
        kfree(src_buf);
        return;
    }
    for (uint32_t k = 0; k < src_size; k++) source[k] = (char)src_buf[k];
    source[src_size] = '\0';
    kfree(src_buf);

    console_printf(sh->con, "Compiling '%s' (%u bytes)...\n", file_path, src_size);

    /* Compile */
    void *entry = NULL;
    if (tcc_kernel_compile(source, &entry) != 0) {
        console_printf(sh->con, "cc: compilation failed\n");
        kfree(source);
        return;
    }

    kfree(source);

    /* Run */
    console_printf(sh->con, "Running...\n");
    int result = tcc_kernel_run(entry);
    console_printf(sh->con, "Exit code: %d\n", result);
}

void shell_tcc_dispatch(SHELL *sh) {
    if (str_starts_with_cc(sh->input, "cc ")) {
        shell_run_cc(sh, sh->input + 3);
        return;
    }
    if (str_eq_cc(sh->input, "cc")) {
        console_printf(sh->con, "Usage: cc <partition> <file.c>\n");
        console_printf(sh->con, "  Compiles and runs a C source file from a FAT32 partition.\n");
        console_printf(sh->con, "  Example: cc sd0p1 hello.c\n");
        return;
    }
}

/* --------------------------------------------------------------------------
 * cc3 — compile a ring 3 userspace ELF with the in-kernel TCC
 *
 * Reads <file.c> from a FAT32 partition, compiles it into a full ELF64
 * executable linked at USER_BASE (ring 3 image, loadable by `run`), and
 * writes it next to the source as <stem>.elf.
 *
 * Usage: cc3 <partition> <file.c>
 *   Example: cc3 sd0p1 hello3.c        -> /users/hello3.elf (run hello3.elf)
 * -------------------------------------------------------------------------- */

extern int tcc_kernel_compile_elf(CONSOLE *con, const char *source,
                                  uint8_t **out_img, uint32_t *out_size);

static void cc3_build_outname(const char *src, char *out, uint32_t cap) {
    /* strip leading spaces */
    while (*src == ' ') src++;

    /* copy directory part if any */
    const char *last_slash = NULL;
    for (const char *p = src; *p; p++) {
        if (*p == '/') last_slash = p;
    }

    uint32_t n = 0;
    if (last_slash) {
        const char *p = src;
        while (p <= last_slash && n + 1 < cap) out[n++] = *p++;
    }

    /* stem without extension, after the slash */
    const char *s = last_slash ? last_slash + 1 : src;
    const char *dot = NULL;
    {
        const char *q = s;
        while (*q) { if (*q == '.') dot = q; q++; }
    }

    while (*s && (dot ? s < dot : 1) && n + 1 < cap) out[n++] = *s++;

    const char *ext = ".elf";
    while (*ext && n + 1 < cap) out[n++] = *ext++;
    out[n] = '\0';
}

void shell_run_cc3(SHELL *sh, const char *args) {
    char part_name[32];
    char file_path[128];
    char out_path[160];
    uint32_t i = 0;
    uint32_t j = 0;

    while (args[i] == ' ') i++;
    while (args[i] && args[i] != ' ' && j + 1 < sizeof(part_name))
        part_name[j++] = args[i++];
    part_name[j] = '\0';

    if (!part_name[0]) {
        console_printf(sh->con, "Usage: cc3 <partition> <file.c>\n");
        console_printf(sh->con, "  Ring 3 compiler: file.c -> file.elf, then run file.elf\n");
        console_printf(sh->con, "  Example: cc3 sd0p1 hello3.c\n");
        return;
    }

    while (args[i] == ' ') i++;
    j = 0;
    while (args[i] && args[i] != ' ' && j + 1 < sizeof(file_path))
        file_path[j++] = args[i++];
    file_path[j] = '\0';

    if (!file_path[0]) {
        console_printf(sh->con, "Usage: cc3 <partition> <file.c>\n");
        return;
    }

    /* resolve relative paths against the shell cwd */
    char resolved_path[160];
    shell_resolve_cwd_path(file_path, resolved_path, sizeof(resolved_path));

    PARTITION_INFO *part = cc_find_partition(sh, part_name);
    if (!part) return;

    uint8_t *src_buf = NULL;
    uint32_t src_size = 0;

    if (!fat32_read_file(part, resolved_path, &src_buf, &src_size)) {
        console_printf(sh->con, "cc3: cannot read '%s' on %s\n", resolved_path, part->name);
        return;
    }

    if (src_size == 0) {
        console_printf(sh->con, "cc3: '%s' is empty\n", resolved_path);
        kfree(src_buf);
        return;
    }

    char *source = kmalloc(src_size + 1);
    if (!source) {
        console_printf(sh->con, "cc3: out of memory\n");
        kfree(src_buf);
        return;
    }
    for (uint32_t k = 0; k < src_size; k++) source[k] = (char)src_buf[k];
    source[src_size] = '\0';
    kfree(src_buf);

    console_printf(sh->con, "cc3: compiling '%s' (%u bytes) for ring 3...\n",
                   resolved_path, src_size);

    uint8_t *img = NULL;
    uint32_t img_size = 0;

    if (tcc_kernel_compile_elf(sh->con, source, &img, &img_size) != 0) {
        console_printf(sh->con, "cc3: compilation failed\n");
        kfree(source);
        return;
    }

    kfree(source);
    console_printf(sh->con, "cc3: image ready (%u bytes)\n", img_size);

    cc3_build_outname(resolved_path, out_path, sizeof(out_path));

    /* create the target if brand new (write_file overwrites existing) */
    fat32_create_file(sh->con, part, out_path);

    if (!fat32_write_file(sh->con, part, out_path, img, img_size)) {
        console_printf(sh->con, "cc3: cannot write '%s' on %s\n", out_path, part->name);
        kfree(img);
        return;
    }
    kfree(img);

    console_printf(sh->con, "cc3: wrote '%s' — run it with: run %s\n",
                   out_path, out_path);
}
