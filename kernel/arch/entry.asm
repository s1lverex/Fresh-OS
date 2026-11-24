; Fresh OS kernel entry. Both loaders jump here in long mode with
; RDI = physical (identity mapped) pointer to struct boot_info.
default rel
bits 64

section .text.entry
global _start
extern kmain
extern __bss_start
extern __bss_end

_start:
    cli
    cld
    mov r12, rdi
    lea rdi, [__bss_start]
    lea rcx, [__bss_end]
    sub rcx, rdi
    xor eax, eax
    rep stosb
    lea rsp, [boot_stack_top]
    xor ebp, ebp
    mov rdi, r12
    call kmain
.hang:
    cli
    hlt
    jmp .hang

section .bss
align 16
global boot_stack_bottom
global boot_stack_top
boot_stack_bottom:
    resb 32768
boot_stack_top:
