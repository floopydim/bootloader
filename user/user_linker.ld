/*
 * User program link layout.
 *
 * Everything loads inside the private user range (PML4 index 1 in the
 * per-task address space). Segments are page-aligned so the kernel
 * loader can map each section with its own protection flags without
 * aliasing pages.
 */

OUTPUT_FORMAT(elf64-x86-64)
ENTRY(_start)

SECTIONS
{
    . = 0x0000008000000000;

    .text : ALIGN(0x1000) {
        *(.ltext*)
        *(.text*)
    }

    .rodata : ALIGN(0x1000) {
        *(.lrodata*)
        *(.rodata*)
    }

    .data : ALIGN(0x1000) {
        *(.ldata*)
        *(.data*)
    }

    .bss : ALIGN(0x1000) {
        *(.bss*)
        *(COMMON)
    }

    /DISCARD/ : {
        *(.eh_frame)
        *(.eh_frame_hdr)
        *(.comment)
    }
}
