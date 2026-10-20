BITS 64
default rel

section .text

global _start
extern main

; Kernel handoff: userspace starts with all GP regs zero and RSP set
; to the top of the user stack by the kernel; here we just call main()
; and exit through the syscall gate (INT 0x80, SYS_EXIT).
_start:
    call main

    ; SYS_EXIT(main_result)
    mov rdi, rax
    xor rsi, rsi
    xor rdx, rdx
    mov eax, 0
    int 0x80

.hang:
    jmp .hang
