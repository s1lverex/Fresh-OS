; Fresh OS - BIOS stage 2.
; Loaded by stage 1 at 0000:8000 (max 32 KiB). Real mode work:
;   check CPU, enable A20, read the E820 memory map, load KERNEL.BIN and the
;   initrd from disk (via unreal mode copies above 1 MiB), pick a VBE linear
;   framebuffer mode, build page tables, enter long mode and jump to the
;   kernel with RDI = boot_info.
;
; Physical layout used here:
;   0x00800  boot_info            0x01000  E820 entries (max 128)
;   0x02000  VBE info block       0x02200  VBE mode info
;   0x07C00  stack top            0x08000  stage 2
;   0x10000  PML4                 0x11000  PDPT (low)
;   0x12000  4 x PD (identity map of first 4 GiB, 2 MiB pages)
;   0x16000  PDPT (high)          0x17000  PD (kernel at -2 GiB)
;   0x20000  disk bounce buffer (32 KiB)
;   0x1FFFC0 KERNEL.BIN (header is 64 bytes, image lands at 0x200000)

[BITS 16]
[ORG 0x8000]

BOOTINFO    equ 0x0800
E820_BUF    equ 0x1000
E820_MAX    equ 128
VBE_INFO    equ 0x2000
VBE_MODE    equ 0x2200
PML4        equ 0x10000
PDPT_LO     equ 0x11000
PD_LO       equ 0x12000
PDPT_HI     equ 0x16000
PD_HI       equ 0x17000
BOUNCE_SEG  equ 0x2000
BOUNCE      equ 0x20000
KERNEL_PHYS equ 0x200000
KERNEL_LOAD equ KERNEL_PHYS - 64

BOOTINFO_MAGIC_LO equ 0xC0DE0001
BOOTINFO_MAGIC_HI equ 0xF2E5B007

; ---------------------------------------------------------------- header
hdr:
    jmp near real_start
    times 8-($-hdr) db 0
    db "FRSHSTG2"              ; +8   magic, patched fields follow
h_kernel_lba:     dq 0         ; +16
h_kernel_sectors: dd 0         ; +24
h_initrd_sectors: dd 0         ; +28
h_initrd_lba:     dq 0         ; +32
h_initrd_bytes:   dd 0         ; +40
h_reserved:       dd 0         ; +44
h_cmdline:        times 128 db 0 ; +48

; ---------------------------------------------------------------- code
real_start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    sti
    cld
    mov [boot_drive], dl

    mov si, msg_hello
    call print

    ; ---- CPU must support long mode
    pushfd
    pop eax
    mov ecx, eax
    xor eax, 0x200000          ; toggle ID bit -> CPUID present?
    push eax
    popfd
    pushfd
    pop eax
    push ecx
    popfd
    xor eax, ecx
    jz .no_lm
    mov eax, 0x80000000
    cpuid
    cmp eax, 0x80000001
    jb .no_lm
    mov eax, 0x80000001
    cpuid
    test edx, 1 << 29
    jnz .lm_ok
.no_lm:
    mov si, msg_nolm
    jmp fatal
.lm_ok:

    ; ---- A20
    mov ax, 0x2401
    int 0x15
    in al, 0x92
    test al, 2
    jnz .a20_set
    or al, 2
    and al, 0xFE
    out 0x92, al
.a20_set:
    call check_a20
    jnc .a20_ok
    call a20_kbc
    call check_a20
    jnc .a20_ok
    mov si, msg_a20
    jmp fatal
.a20_ok:

    ; ---- clear boot_info
    mov di, BOOTINFO
    mov cx, 256/2
    xor ax, ax
    rep stosw

    ; ---- E820 memory map
    mov di, E820_BUF
    xor ebx, ebx
    xor bp, bp
.e820_next:
    mov dword [di+20], 1
    mov eax, 0xE820
    mov edx, 0x534D4150
    mov ecx, 24
    int 0x15
    jc .e820_done
    cmp eax, 0x534D4150
    jne .e820_done
    cmp cl, 20
    ja .e820_attr
    mov dword [di+20], 1
.e820_attr:
    test byte [di+20], 1
    jz .e820_skip
    mov eax, [di+8]
    or eax, [di+12]
    jz .e820_skip
    inc bp
    add di, 24
    cmp bp, E820_MAX
    jae .e820_done
.e820_skip:
    test ebx, ebx
    jnz .e820_next
.e820_done:
    test bp, bp
    jnz .e820_ok
    mov si, msg_e820
    jmp fatal
.e820_ok:
    mov [BOOTINFO+96], bp      ; mmap_count (low word; high word already 0)

    ; ---- load KERNEL.BIN to KERNEL_LOAD
    mov si, msg_kernel
    call print
    mov eax, [h_kernel_lba]
    mov [ld_lba], eax
    mov eax, [h_kernel_lba+4]
    mov [ld_lba+4], eax
    mov eax, [h_kernel_sectors]
    mov [ld_count], eax
    mov dword [ld_dest], KERNEL_LOAD
    call load_sectors

    call enter_unreal
    mov esi, KERNEL_LOAD
    cmp dword [esi], 0x48535246 ; "FRSH" of "FRSHKRNL"
    jne .bad_kernel
    cmp dword [esi+4], 0x4C4E524B ; "KRNL"
    jne .bad_kernel
    mov eax, [esi+8]           ; entry (64 bit)
    mov [kernel_entry], eax
    mov eax, [esi+12]
    mov [kernel_entry+4], eax
    mov eax, [esi+16]          ; load_size (low 32 bits suffice)
    mov [kernel_load_size], eax
    mov eax, [esi+24]          ; mem_size
    mov [kernel_mem_size], eax
    cmp eax, 0x4000000         ; sanity: < 64 MiB
    jae .bad_kernel
    jmp .kernel_ok
.bad_kernel:
    mov si, msg_badkernel
    jmp fatal
.kernel_ok:

    ; ---- load initrd right after the kernel's memory image (4 KiB aligned)
    mov eax, [kernel_mem_size]
    add eax, KERNEL_PHYS + 0xFFF
    and eax, 0xFFFFF000
    mov [initrd_phys], eax
    mov eax, [h_initrd_sectors]
    test eax, eax
    jz .no_initrd
    mov si, msg_initrd
    call print
    mov [ld_count], eax
    mov eax, [h_initrd_lba]
    mov [ld_lba], eax
    mov eax, [h_initrd_lba+4]
    mov [ld_lba+4], eax
    mov eax, [initrd_phys]
    mov [ld_dest], eax
    call load_sectors
.no_initrd:

    ; ---- zero the kernel bss (memory between load_size and mem_size)
    call enter_unreal
    mov edi, [kernel_load_size]
    add edi, KERNEL_PHYS
    mov ecx, [kernel_mem_size]
    sub ecx, [kernel_load_size]
    jbe .bss_done
    xor al, al
    a32 rep stosb
.bss_done:

    ; ---- fill boot_info (non framebuffer parts)
    mov dword [BOOTINFO+0], BOOTINFO_MAGIC_LO
    mov dword [BOOTINFO+4], BOOTINFO_MAGIC_HI
    mov dword [BOOTINFO+8], 1          ; version
    mov dword [BOOTINFO+12], 1         ; boot_type BIOS
    mov dword [BOOTINFO+16], KERNEL_PHYS
    mov eax, [kernel_mem_size]
    mov [BOOTINFO+24], eax
    mov eax, [initrd_phys]
    mov [BOOTINFO+32], eax
    mov eax, [h_initrd_bytes]
    mov [BOOTINFO+40], eax
    mov dword [BOOTINFO+88], E820_BUF  ; mmap_addr
    mov dword [BOOTINFO+100], 24       ; mmap_entry_size
    mov si, h_cmdline
    mov di, BOOTINFO+120
    mov cx, 127
    rep movsb

    ; ---- VBE: choose the best 32 bpp linear mode (prefer 1024x768)
    call setup_vbe

    ; ---- page tables
    call enter_unreal
    mov edi, PML4
    mov ecx, 0x8000/4
    xor eax, eax
    a32 rep stosd
    mov edi, PML4
    mov dword [edi], PDPT_LO | 3
    mov dword [edi + 511*8], PDPT_HI | 3
    mov edi, PDPT_LO
    mov eax, PD_LO | 3
    mov cx, 4
.pdpt_lo:
    mov [edi], eax
    add eax, 0x1000
    add edi, 8
    loop .pdpt_lo
    mov edi, PD_LO
    mov eax, 0x83
    mov ecx, 2048
.pd_lo:
    mov [edi], eax
    add eax, 0x200000
    add edi, 8
    a32 loop .pd_lo
    mov edi, PDPT_HI
    mov dword [edi + 510*8], PD_HI | 3
    mov edi, PD_HI
    mov eax, KERNEL_PHYS | 0x83
    mov ecx, 64
.pd_hi:
    mov [edi], eax
    add eax, 0x200000
    add edi, 8
    a32 loop .pd_hi

    ; ---- enter long mode
    cli
    mov al, 0xFF               ; mask both PICs
    out 0xA1, al
    out 0x21, al
    lgdt [gdt_desc]
    mov eax, cr4
    or eax, 1 << 5             ; PAE
    mov cr4, eax
    mov eax, PML4
    mov cr3, eax
    mov ecx, 0xC0000080        ; EFER
    rdmsr
    or eax, 1 << 8             ; LME
    wrmsr
    mov eax, cr0
    or eax, 0x80000001         ; PG | PE
    mov cr0, eax
    jmp 0x08:long_mode

; ---------------------------------------------------------------- helpers
; print zero terminated string at DS:SI (teletype)
print:
    pusha
.next:
    lodsb
    test al, al
    jz .done
    mov ah, 0x0E
    mov bx, 0x0007
    int 0x10
    jmp .next
.done:
    popa
    ret

fatal:
    call print
.halt:
    cli
    hlt
    jmp .halt

; CF clear if A20 is enabled
check_a20:
    push ds
    push es
    push si
    push di
    xor ax, ax
    mov ds, ax
    not ax
    mov es, ax                 ; ES = 0xFFFF
    mov si, 0x0500
    mov di, 0x0510
    mov al, [ds:si]
    push ax
    mov al, [es:di]
    push ax
    mov byte [ds:si], 0x00
    mov byte [es:di], 0xFF
    cmp byte [ds:si], 0xFF
    pop ax
    mov [es:di], al
    pop ax
    mov [ds:si], al
    clc
    jne .ok
    stc
.ok:
    pop di
    pop si
    pop es
    pop ds
    ret

; enable A20 through the keyboard controller
a20_kbc:
    cli
    call .wait_in
    mov al, 0xD1
    out 0x64, al
    call .wait_in
    mov al, 0xDF
    out 0x60, al
    call .wait_in
    sti
    ret
.wait_in:
    mov cx, 0xFFFF
.w:
    in al, 0x64
    test al, 2
    loopnz .w
    ret

; switch DS/ES to 4 GiB limit while staying in real mode
enter_unreal:
    pushad
    cli
    push ds
    push es
    lgdt [gdt_desc]
    mov eax, cr0
    or al, 1
    mov cr0, eax
    jmp short .pm
.pm:
    mov bx, 0x10
    mov ds, bx
    mov es, bx
    and al, 0xFE
    mov cr0, eax
    jmp short .rm
.rm:
    pop es
    pop ds
    sti
    popad
    ret

; read [ld_count] sectors from LBA [ld_lba] to flat address [ld_dest]
load_sectors:
.loop:
    mov eax, [ld_count]
    test eax, eax
    jz .done
    cmp eax, 64
    jbe .chunk
    mov eax, 64
.chunk:
    mov [cur_chunk], eax
    mov byte [retries], 3
.retry:
    mov ax, [cur_chunk]
    mov [dap_count], ax
    mov word [dap_off], 0
    mov word [dap_seg], BOUNCE_SEG
    mov eax, [ld_lba]
    mov [dap_lba], eax
    mov eax, [ld_lba+4]
    mov [dap_lba+4], eax
    mov si, dap
    mov ah, 0x42
    mov dl, [boot_drive]
    int 0x13
    jnc .ok
    dec byte [retries]
    jz .fail
    xor ah, ah                 ; reset disk and retry
    mov dl, [boot_drive]
    int 0x13
    jmp .retry
.ok:
    call enter_unreal
    mov ecx, [cur_chunk]
    shl ecx, 7                 ; sectors * 128 dwords
    mov esi, BOUNCE
    mov edi, [ld_dest]
    a32 rep movsd
    mov [ld_dest], edi
    mov eax, [cur_chunk]
    add [ld_lba], eax
    adc dword [ld_lba+4], 0
    sub [ld_count], eax
    mov al, '.'
    mov ah, 0x0E
    mov bx, 7
    int 0x10
    jmp .loop
.done:
    mov si, msg_crlf
    call print
    ret
.fail:
    mov si, msg_disk
    jmp fatal

; find and set the best VBE mode, fill framebuffer fields of boot_info
setup_vbe:
    mov di, VBE_INFO
    mov dword [di], 0x32454256 ; "VBE2"
    mov ax, 0x4F00
    int 0x10
    cmp ax, 0x004F
    jne .none
    cmp dword [VBE_INFO], 0x41534556 ; "VESA"
    jne .none
    mov dword [best_score], 0
    mov word [best_mode], 0xFFFF
    mov si, [VBE_INFO+14]
    mov ax, [VBE_INFO+16]
    mov [list_seg], ax
.mode_loop:
    push ds
    mov ds, [list_seg]
    mov cx, [si]
    pop ds
    cmp cx, 0xFFFF
    je .chosen
    add si, 2
    mov [cur_mode], cx
    push si
    mov ax, 0x4F01
    mov di, VBE_MODE
    int 0x10
    pop si
    cmp ax, 0x004F
    jne .mode_loop
    mov ax, [VBE_MODE+0]
    and ax, 0x91
    cmp ax, 0x91               ; supported, graphics, linear
    jne .mode_loop
    cmp byte [VBE_MODE+25], 32
    jne .mode_loop
    cmp byte [VBE_MODE+27], 6  ; direct colour
    jne .mode_loop
    cmp dword [VBE_MODE+40], 0
    je .mode_loop
    movzx eax, word [VBE_MODE+18]
    movzx edx, word [VBE_MODE+20]
    cmp eax, 640
    jb .mode_loop
    cmp eax, 1024
    jne .not_pref
    cmp edx, 768
    jne .not_pref
    mov eax, 0xFFFFFFFF
    jmp .score
.not_pref:
    cmp eax, 1280
    ja .mode_loop
    cmp edx, 1024
    ja .mode_loop
    imul eax, edx
.score:
    cmp eax, [best_score]
    jbe .mode_loop
    mov [best_score], eax
    mov ax, [cur_mode]
    mov [best_mode], ax
    jmp .mode_loop
.chosen:
    cmp word [best_mode], 0xFFFF
    je .none
    mov cx, [best_mode]
    mov ax, 0x4F01
    mov di, VBE_MODE
    int 0x10
    cmp ax, 0x004F
    jne .none
    mov bx, [best_mode]
    or bx, 0x4000
    mov ax, 0x4F02
    int 0x10
    cmp ax, 0x004F
    jne .none
    mov eax, [VBE_MODE+40]
    mov [BOOTINFO+48], eax
    movzx eax, word [VBE_MODE+18]
    mov [BOOTINFO+56], eax
    movzx eax, word [VBE_MODE+20]
    mov [BOOTINFO+60], eax
    movzx eax, word [VBE_MODE+16]
    cmp word [VBE_INFO+4], 0x0300
    jb .pitch
    movzx eax, word [VBE_MODE+50]
.pitch:
    mov [BOOTINFO+64], eax
    mov dword [BOOTINFO+68], 32
    mov al, [VBE_MODE+32]
    mov [BOOTINFO+72], al
    mov al, [VBE_MODE+31]
    mov [BOOTINFO+73], al
    mov al, [VBE_MODE+34]
    mov [BOOTINFO+74], al
    mov al, [VBE_MODE+33]
    mov [BOOTINFO+75], al
    mov al, [VBE_MODE+36]
    mov [BOOTINFO+76], al
    mov al, [VBE_MODE+35]
    mov [BOOTINFO+77], al
    mov byte [BOOTINFO+78], 1  ; BOOT_FB_RGB
    ret
.none:
    mov byte [BOOTINFO+78], 0
    ret

; ---------------------------------------------------------------- 64 bit
[BITS 64]
long_mode:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    xor eax, eax
    mov fs, ax
    mov gs, ax
    mov rsp, 0x7C00
    mov rdi, BOOTINFO
    mov rax, [kernel_entry]
    jmp rax

; ---------------------------------------------------------------- data
[BITS 16]
align 8
gdt:
    dq 0
    dq 0x00AF9A000000FFFF      ; 0x08: 64-bit code
    dq 0x00CF92000000FFFF      ; 0x10: flat data
gdt_end:
gdt_desc:
    dw gdt_end - gdt - 1
    dd gdt

align 4
dap:
    db 16, 0
dap_count: dw 0
dap_off:   dw 0
dap_seg:   dw 0
dap_lba:   dq 0

align 8
kernel_entry:     dq 0
kernel_load_size: dd 0
kernel_mem_size:  dd 0
initrd_phys:      dd 0
ld_lba:           dq 0
ld_count:         dd 0
ld_dest:          dd 0
cur_chunk:        dd 0
best_score:       dd 0
best_mode:        dw 0
cur_mode:         dw 0
list_seg:         dw 0
boot_drive:       db 0
retries:          db 0

msg_hello:     db "Fresh OS boot (BIOS)", 13, 10, 0
msg_kernel:    db "Loading kernel", 0
msg_initrd:    db "Loading initrd", 0
msg_crlf:      db 13, 10, 0
msg_nolm:      db "Fresh OS needs a 64-bit (x86-64) CPU.", 0
msg_a20:       db "Cannot enable the A20 line.", 0
msg_e820:      db "No E820 memory map from BIOS.", 0
msg_disk:      db 13, 10, "Disk read error.", 0
msg_badkernel: db "KERNEL.BIN is invalid.", 0

times 32768-($-$$) db 0xCC
