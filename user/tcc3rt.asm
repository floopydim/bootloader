; ---------------------------------------------------------------------------
; tcc3rt.asm — CFOS ring 3 runtime for in-kernel TCC (cc "ring 3" output).
;
; cc3 links this object into every program it produces. It provides:
;   - the process entry (_start): calls main(), then exits with its return
;     value through the INT 0x80 gate;
;   - syscall wrappers (the kernel TCC build has no inline assembler, so
;     the machine code lives here, once);
;   - TCC helper functions: memcpy/memmove/memset are emitted by tccgen for
;     struct copies and initializations; they must exist under these names;
;   - small printf-ish helpers so user programs can print without hand
;     rolling conversion code.
;
; Built with: nasm -f elf64 user/tcc3rt.asm -o build/ccrt.o
; The .o is embedded into the kernel binary and cc3 feeds it to TCC as a
; normal relocatable object through the RAM-file shim.
;
; Syscall contract (kernel/userproc.c):
;   RAX = syscall number, RDI/RSI/RDX = args, RAX = result.
; ---------------------------------------------------------------------------

BITS 64
default rel

SYS_EXIT    equ 0
SYS_READ    equ 1
SYS_WRITE   equ 2
SYS_GETPID  equ 5
SYS_GETTIME equ 12

section .text

global _start
global s_exit
global s_read
global s_write
global s_puts
global s_nl
global getchar
global print_i64
global print_u64
global print_hex64
global getpid
global uptime
global memcpy
global memmove
global memset

extern main

; ---------------------------------------------------------------------------
; _start — image entry point.
; ---------------------------------------------------------------------------
_start:
    cld
    sub rsp, 8                  ; 16-byte-ish alignment for the call
    call main
    mov  rdi, rax               ; exit code of main
    mov  eax, SYS_EXIT
    int 0x80
.hang:
    jmp .hang

; ---------------------------------------------------------------------------
; void s_exit(int code) — noreturn
; ---------------------------------------------------------------------------
s_exit:
    mov  eax, SYS_EXIT
    int 0x80
.l1:
    jmp .l1

; ---------------------------------------------------------------------------
; unsigned long s_write(const void *buf, unsigned long n)
; ---------------------------------------------------------------------------
s_write:
    mov  rdx, rsi               ; count
    mov  rsi, rdi               ; buf
    mov  edi, 1                 ; fd 1 = console
    mov  eax, SYS_WRITE
    int 0x80
    ret

; ---------------------------------------------------------------------------
; unsigned long s_read(void *buf, unsigned long n) — blocking, fd 0
; ---------------------------------------------------------------------------
s_read:
    mov  rdx, rsi               ; count
    mov  rsi, rdi               ; buf
    xor  edi, edi               ; fd 0 = console
    mov  eax, SYS_READ
    int 0x80
    ret

; ---------------------------------------------------------------------------
; void s_puts(const char *s)
; ---------------------------------------------------------------------------
s_puts:
    mov  rsi, rdi
    xor  rdx, rdx
.l2:
    cmp  byte [rsi + rdx], 0
    je   .d2
    inc  rdx
    jmp  .l2
.d2:
    test rdx, rdx
    jz   .z2
    mov  rdi, 1
    mov  eax, SYS_WRITE
    int 0x80
.z2:
    ret

; ---------------------------------------------------------------------------
; void s_nl(void)
; ---------------------------------------------------------------------------
s_nl:
    sub  rsp, 8
    mov  byte [rsp], 0x0a
    mov  rdi, 1                 ; fd
    mov  rsi, rsp               ; buf
    mov  edx, 1                 ; count
    mov  eax, SYS_WRITE
    int 0x80
    add  rsp, 8
    ret

; ---------------------------------------------------------------------------
; int getchar(void) — blocking, -1 on error
; ---------------------------------------------------------------------------
getchar:
    sub  rsp, 16
    mov  byte [rsp], 0
    xor  edi, edi
    mov  rsi, rsp
    mov  edx, 1
    mov  eax, SYS_READ
    int 0x80
    cmp  rax, 1
    jne  .e3
    movsx eax, byte [rsp]
    add  rsp, 16
    ret
.e3:
    mov  eax, -1
    add  rsp, 16
    ret

; ---------------------------------------------------------------------------
; unsigned long getpid(void)
; ---------------------------------------------------------------------------
getpid:
    mov  eax, SYS_GETPID
    int 0x80
    ret

; ---------------------------------------------------------------------------
; unsigned long uptime(void) — kernel uptime in seconds
; ---------------------------------------------------------------------------
uptime:
    mov  eax, SYS_GETTIME
    int 0x80
    ret

; ---------------------------------------------------------------------------
; void print_i64(long v) — signed decimal
;   digits are generated LSD-first in a 24-byte stack buffer, then printed
;   straight from the buffer: printing reverses nothing, so walk the buffer
;   backwards via pointer arithmetic in the syscall call. The kernel byte
;   copy loop does not need contiguity from the middle, only a start
;   pointer, so we instead copy the digits into printing order with a tiny
;   reverse in place (<= 21 bytes, safe).
; ---------------------------------------------------------------------------
print_i64:
    sub  rsp, 32
    mov  rax, rdi
    mov  r11, 0                 ; negative flag
    test rax, rax
    jns  .mag4
    mov  r11, 1
    neg  rax
.mag4:
    xor  rcx, rcx               ; digit count
    mov  r10, 10
.l4:
    xor  edx, edx
    div  r10
    add  dl, '0'
    mov  [rsp + rcx], dl
    inc  rcx
    test rax, rax
    jnz  .l4
    ; reverse in place: buffer[i] <-> buffer[rcx-1-i], meet in middle
    xor  r9, r9                 ; low index
    mov  r8, rcx
    dec  r8                     ; high index
.r4:
    cmp  r9, r8
    jae  .rd4
    mov  al, [rsp + r9]
    mov  bl, [rsp + r8]
    mov  [rsp + r9], bl
    mov  [rsp + r8], al
    inc  r9
    dec  r8
    jmp  .r4
.rd4:
    ; digits are MSD-first in [rsp .. rsp+rcx-1] now
    test r11, r11
    jz   .p4
    mov  byte [rsp + 21], '-'
    mov  rsi, rsp
    add  rsi, 21
    mov  rdi, 1
    mov  edx, 1
    mov  eax, SYS_WRITE
    int 0x80
.p4:
    mov  rsi, rsp
    mov  rdx, rcx
    mov  rdi, 1
    mov  eax, SYS_WRITE
    int 0x80
    add  rsp, 32
    ret

; ---------------------------------------------------------------------------
; void print_u64(unsigned long v)
; ---------------------------------------------------------------------------
print_u64:
    sub  rsp, 32
    mov  rax, rdi
    xor  rcx, rcx
    mov  r10, 10
.l5:
    xor  edx, edx
    div  r10
    add  dl, '0'
    mov  [rsp + rcx], dl
    inc  rcx
    test rax, rax
    jnz  .l5
    xor  r9, r9                 ; low index
    mov  r8, rcx
    dec  r8                     ; high index
.r5:
    cmp  r9, r8
    jae  .d5
    mov  al, [rsp + r9]
    mov  bl, [rsp + r8]
    mov  [rsp + r9], bl
    mov  [rsp + r8], al
    inc  r9
    dec  r8
    jmp  .r5
.d5:
    mov  rsi, rsp
    mov  rdx, rcx
    mov  rdi, 1
    mov  eax, SYS_WRITE
    int 0x80
    add  rsp, 32
    ret

; ---------------------------------------------------------------------------
; void print_hex64(unsigned long v) — "0x" + hex digits
; ---------------------------------------------------------------------------
print_hex64:
    sub  rsp, 48
    mov  byte [rsp + 0], '0'
    mov  byte [rsp + 1], 'x'
    mov  rax, rdi
    mov  rcx, 0                 ; digit count (2 = after "0x")
.l6:
    mov  rdx, rax
    and  edx, 0x0F
    cmp  dl, 10
    jb   .lit6
    add  dl, 'A' - 10 - '0'
.lit6:
    add  dl, '0'
    mov  [rsp + rcx + 2], dl
    inc  rcx
    shr  rax, 4
    test rax, rax
    jnz  .l6
    ; digits sit at rsp+2 .. rsp+rcx+1, LSD-first: reverse them
    mov  r9, 2
    mov  r8, rcx
    add  r8, 1
.r6:
    cmp  r9, r8
    jae  .d6
    mov  al, [rsp + r9]
    mov  bl, [rsp + r8]
    mov  [rsp + r9], bl
    mov  [rsp + r8], al
    inc  r9
    dec  r8
    jmp  .r6
.d6:
    mov  rsi, rsp
    mov  rdx, rcx
    add  rdx, 2                 ; '0x' + digits
    mov  rdi, 1
    mov  eax, SYS_WRITE
    int 0x80
    add  rsp, 48
    ret

; ---------------------------------------------------------------------------
; void *memcpy(void *dst, const void *src, unsigned long n)
; ---------------------------------------------------------------------------
memcpy:
    mov  rax, rdi
    mov  rcx, rdx
    cld
    rep  movsb
    ret

; ---------------------------------------------------------------------------
; void *memmove(void *dst, const void *src, unsigned long n)
; ---------------------------------------------------------------------------
memmove:
    mov  rax, rdi
    test rdx, rdx
    jz   .z7
    cmp  rdi, rsi
    je   .z7
    jb   .f7                    ; dst < src: copy forward
    ; dst > src: copy backwards
    mov  rcx, rdx
    dec  rcx
    add  rdi, rcx
    add  rsi, rcx
    inc  rcx
    std
    rep  movsb
    cld
    ret
.f7:
    mov  rcx, rdx
    cld
    rep  movsb
.z7:
    ret

; ---------------------------------------------------------------------------
; void *memset(void *dst, int c, unsigned long n)
; ---------------------------------------------------------------------------
memset:
    mov  rax, rdi
    mov  rcx, rdx
    mov  al, sil
    cld
    rep  stosb
    ret
