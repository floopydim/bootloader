BITS 64
default rel

section .text

global user_enter_asm

; task_context_t layout (matches task.h):
;   +0   rax       +8   rbx    +16  rcx    +24  rdx
;   +32  rsi       +40  rdi    +48  rbp    +56  rsp
;   +64  r8..+120  r15  +128   rip  +136   rflags  +144  cr3
;
; user_frame_t (userspace startup frame):
;   +0 rip  +8 cs  +16 rflags  +24 rsp  +32 ss
;
; void user_enter_asm(task_context_t *save, user_frame_t *frame, uint64_t cr3_new);
;   rdi        = where the kernel context gets saved (restore point)
;   rsi        = ring 3 startup frame
;   rdx        = page table root (physical) of the user address space
user_enter_asm:
    ; ---- save kernel context (exactly like task_switch_asm) ----
    mov [rdi + 0],   rax
    mov [rdi + 8],   rbx
    mov [rdi + 16],  rcx
    mov [rdi + 24],  rdx
    mov [rdi + 32],  rsi
    mov [rdi + 40],  rdi
    mov [rdi + 48],  rbp
    mov [rdi + 56],  rsp
    mov [rdi + 64],  r8
    mov [rdi + 72],  r9
    mov [rdi + 80],  r10
    mov [rdi + 88],  r11
    mov [rdi + 96],  r12
    mov [rdi + 104], r13
    mov [rdi + 112], r14
    mov [rdi + 120], r15

    ; Save RIP (return address of this call)
    mov rax, [rsp]
    mov [rdi + 128], rax

    ; Save RFLAGS / CR3 (note: rflags contains IF, restored on exit)
    pushfq
    pop rax
    mov [rdi + 136], rax

    mov rax, cr3
    mov [rdi + 144], rax

    ; ---- switch to the user address space ----
    mov rax, rdx
    mov cr3, rax

    ; ---- build the ring 3 frame and IRET into it ----
    push qword [rsi + 32]   ; ss
    push qword [rsi + 24]   ; rsp
    push qword [rsi + 16]   ; rflags
    push qword [rsi + 8]    ; cs
    push qword [rsi + 0]    ; rip

    ; Starting register state for userspace: all zero.
    xor eax, eax
    xor ebx, ebx
    xor ecx, ecx
    xor edx, edx
    xor esi, esi
    xor edi, edi
    xor ebp, ebp
    xor r8d,  r8d
    xor r9d,  r9d
    xor r10d, r10d
    xor r11d, r11d
    xor r12d, r12d
    xor r13d, r13d
    xor r14d, r14d
    xor r15d, r15d

    cld
    iretq
