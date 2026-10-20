/*
 * Stack probe for Windows x64 (MSVC) ABI.
 *
 * Contract (MS x64 __chkstk):
 *   - on entry: RAX = frame size in bytes
 *   - must touch (probe) every 4 KiB page between current RSP and
 *     RSP - RAX, so that a dynamically guarded stack would fault early
 *   - must return with RAX = frame size unchanged
 *
 * The previous implementation was broken in every way possible: it
 * popped the return address into RCX, trashed RSP/RAX, and jumped to
 * the caller through a corrupted stack. Any function with a frame
 * larger than 4 KiB (like efi_main, which holds BOOT_CONFIG on the
 * stack) crashed immediately in its own prologue - before executing a
 * single instruction. This is exactly the "triple fault" seen on boot.
 *
 * Probe is read-only: the UEFI boot stack is static and always fully
 * mapped, so reading a page is enough to validate it.
 */

__asm__ (
    ".globl  __chkstk\n"
    "__chkstk:\n"
    "    push %rcx\n"
    "    push %rdx\n"
    "    mov  %rsp, %rdx\n"
    "    add  $16, %rdx\n"       /* rdx = caller RSP (return-addr slot) */
    "    mov  %rdx, %rcx\n"
    "    sub  %rax, %rcx\n"      /* rcx = lowest address of the frame   */
    "1:\n"
    "    sub  $0x1000, %rdx\n"   /* probe one page per step             */
    "    cmp  %rcx, %rdx\n"
    "    jb   2f\n"
    "    mov  (%rdx), %r10\n"    /* read-only probe, no side effects    */
    "    jmp  1b\n"
    "2:\n"
    "    mov  (%rcx), %r10\n"    /* ensure the boundary page is touched */
    "    pop  %rdx\n"
    "    pop  %rcx\n"
    "    ret\n"
    "\n"
    /* keep chkstk.c from being optimized to an empty TU */
    ".def __chkstk;\n"
    ".scl 2;\n"
    ".type 32;\n"
    ".endef\n"
);

/* (no C code: __chkstk is fully implemented in the module asm above) */
