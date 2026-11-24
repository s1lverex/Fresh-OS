; Fresh OS user program entry: the kernel passes argc in RDI, argv in RSI.
bits 64
section .text.start
global _start
extern __libc_start
_start:
    xor ebp, ebp
    and rsp, -16
    call __libc_start
    ud2
