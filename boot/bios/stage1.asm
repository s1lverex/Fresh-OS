; Fresh OS - BIOS stage 1 (MBR boot code, max 440 bytes).
; Loads stage 2 (64 sectors) from the start of the BIOS boot partition
; (LBA 2048) to 0000:8000 using INT 13h extensions and jumps to it.
; The rest of the MBR (disk signature + protective GPT partition entry +
; 0x55AA) is filled in by tools/mkimage.py.

[BITS 16]
[ORG 0x7C00]

STAGE2_LBA     equ 2048
STAGE2_SECTORS equ 64

start:
    cli
    jmp 0x0000:.flush          ; normalise CS:IP to 0000:7Cxx
.flush:
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    sti
    cld
    mov [drive], dl

    mov ah, 0x41               ; INT 13h extensions present?
    mov bx, 0x55AA
    int 0x13
    jc .noext
    cmp bx, 0xAA55
    jne .noext

    mov si, dap
    mov ah, 0x42
    mov dl, [drive]
    int 0x13
    jc .readerr

    mov dl, [drive]
    jmp 0x0000:0x8000

.noext:
    mov si, msg_noext
    jmp fail
.readerr:
    mov si, msg_read
fail:
    lodsb
    test al, al
    jz .halt
    mov ah, 0x0E
    mov bx, 0x0007
    int 0x10
    jmp fail
.halt:
    cli
    hlt
    jmp .halt

align 4
dap:
    db 16, 0
    dw STAGE2_SECTORS
    dw 0x8000, 0x0000
    dq STAGE2_LBA

drive:     db 0
msg_noext: db "Fresh OS: BIOS lacks LBA disk support", 0
msg_read:  db "Fresh OS: disk read error (stage 2)", 0

times 440-($-$$) db 0
