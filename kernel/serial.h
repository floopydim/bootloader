#ifndef SERIAL_H
#define SERIAL_H

#include <stdint.h>

/*
 * COM1 (16550 UART) output.
 *
 * Purpose: mirror the framebuffer console to COM1 so that on real
 * hardware kernel output can be captured on another machine/terminal,
 * and in QEMU it shows up directly via `-serial stdio`.
 *
 * Writing to an UART with no receiver never stalls the CPU (the data
 * simply goes nowhere), so it is always safe to mirror.
 */

void serial_init(void);

/* Low-level: send one byte through COM1. */
void serial_write_char(char c);

/* Only used by the console mirror: sends CRLF for '\n', raw otherwise. */
void serial_mirror_char(char c);

#endif
