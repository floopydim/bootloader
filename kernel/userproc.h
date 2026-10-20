#ifndef USERPROC_H
#define USERPROC_H

#include <stdint.h>
#include "console.h"
#include "interrupts.h"

/*
 * Ring 3 userspace support.
 *
 * One user task runs at a time, synchronously: the shell calls
 * userproc_run() with an ELF image read from FAT32, drops into ring 3
 * via an IRET trampoline, and stays on the kernel stack of the task
 * while the user code is active. System calls come in through INT 0x80
 * (DPL=3 gate) and faults from ring 3 kill the task instead of the
 * whole kernel.
 */

void userproc_init(void);
int  userproc_is_active(void);

/*
 * Load and run a userspace ELF64 image.
 * Returns 0 on success (exit code stored in *out_exit_code), negative
 * on loader errors (-err).
 */
int userproc_run(CONSOLE *con,
                 const char *name,
                 const uint8_t *elf_image,
                 uint64_t image_size,
                 uint64_t *out_exit_code);

/* INT 0x80 handler (dispatched from interrupts.c). */
void userproc_handle_syscall(INTERRUPT_FRAME *frame);

/* CPU exception while CPL == 3: kill the task, return to the shell. */
void userproc_handle_fault(INTERRUPT_FRAME *frame);

#endif
