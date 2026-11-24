; Fresh OS kernel - interrupt stubs, syscall entry, context switch and
; fault-safe user memory copy routines.
default rel
bits 64

section .text
extern isr_dispatch
extern syscall_dispatch

; ------------------------------------------------------------- stubs
%assign i 0
%rep 48
isr_stub_%[i]:
%if i == 8 || i == 10 || i == 11 || i == 12 || i == 13 || i == 14 || i == 17 || i == 21 || i == 29 || i == 30
%else
    push 0
%endif
    push i
    jmp isr_common
%assign i i+1
%endrep
isr_stub_255:
    push 0
    push 255
    jmp isr_common

isr_common:
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
    cld
    mov rdi, rsp
    call isr_dispatch
global isr_return
isr_return:
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax
    add rsp, 16
    iretq

section .rodata
global isr_stub_table
isr_stub_table:
%assign i 0
%rep 48
    dq isr_stub_%[i]
%assign i i+1
%endrep
global isr_stub_spurious
isr_stub_spurious:
    dq isr_stub_255

section .text
; ------------------------------------------------------------- syscall
global syscall_entry
global cpu_kstack_top
syscall_entry:
    mov [syscall_user_rsp], rsp
    mov rsp, [cpu_kstack_top]
    push 0x1B                  ; ss
    push qword [syscall_user_rsp]
    push r11                   ; rflags
    push 0x23                  ; cs
    push rcx                   ; rip
    push 0                     ; error
    push 0x80                  ; vector
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
    cld
    mov rdi, rsp
    sti
    call syscall_dispatch
    cli
    jmp isr_return

; ------------------------------------------------------------- context
; void switch_context(uint64_t *save_rsp, uint64_t new_rsp)
global switch_context
switch_context:
    push rbp
    push rbx
    push r12
    push r13
    push r14
    push r15
    mov [rdi], rsp
    mov rsp, rsi
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret

global user_entry_trampoline
user_entry_trampoline:
    cli
    jmp isr_return

global fpu_save
fpu_save:
    fxsave64 [rdi]
    ret

global fpu_restore
fpu_restore:
    fxrstor64 [rdi]
    ret

; void gdt_load(void *gdtr, uint16_t tss_sel)
global gdt_load
gdt_load:
    lgdt [rdi]
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    xor eax, eax
    mov fs, ax
    mov gs, ax
    push 0x08
    lea rax, [.reload]
    push rax
    retfq
.reload:
    mov ax, si
    ltr ax
    ret

; ------------------------------------------------------------- user copy
; Faults inside [copy_user_begin, copy_user_end) are redirected by the page
; fault handler to copy_user_fault, which returns -EFAULT to the caller.
global copy_user_begin
global copy_user_end
global copy_user_fault
global copy_user_asm
global strncpy_user_asm

; long copy_user_asm(void *dst, const void *src, size_t n)
copy_user_begin:
copy_user_asm:
    mov rcx, rdx
    rep movsb
    xor eax, eax
    ret

; long strncpy_user_asm(char *dst, const char *src, size_t max)
strncpy_user_asm:
    xor eax, eax
.loop:
    cmp rax, rdx
    jae .toolong
    mov cl, [rsi + rax]
    mov [rdi + rax], cl
    test cl, cl
    jz .done
    inc rax
    jmp .loop
.done:
    ret
.toolong:
    mov rax, -36
    ret
copy_user_end:

copy_user_fault:
    mov rax, -14
    ret

section .bss
align 8
syscall_user_rsp: resq 1
cpu_kstack_top:   resq 1
