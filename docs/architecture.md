# Fresh OS architecture

## Source layout

| Path | Contents |
|---|---|
| `boot/bios/` | `stage1.asm` (MBR, 440 bytes) and `stage2.asm` (real mode to long mode) |
| `boot/uefi/` | `main.c` + `efi.h`: the UEFI loader `BOOTX64.EFI` |
| `include/fresh/` | headers shared by loaders, kernel and user space (`bootinfo.h`, `abi.h`, `str.h`) |
| `kernel/arch/` | entry, GDT/TSS, IDT and exceptions, 8259 PIC, PIT timer, CPU setup, assembly stubs |
| `kernel/mm/` | physical page allocator, page tables, kernel heap |
| `kernel/dev/` | console, font, serial, keyboard, mouse, input events, framebuffer hand-over, terminals and pseudo-terminals, PCI, AHCI, IDE, GPT, ACPI, RTC |
| `kernel/fs/` | VFS, ramfs, initrd unpacker, pipes, FreshFS glue |
| `kernel/proc/` | processes, scheduler, ELF loader, system calls |
| `kernel/test/` | in-kernel self tests |
| `lib/common/` | `string.c` and `fmt.c` (printf engine), used by kernel and libc |
| `lib/freshfs/` | the portable FreshFS implementation and checker, used by kernel and host tools |
| `user/libc/` | C library: syscall wrappers, malloc, stdio, strings |
| `user/bin/` | the programs in `/bin` |
| `user/desktop/` | the graphical desktop `/bin/desktop` |
| `user/test/` | test programs in `/test` |
| `tools/` | image builders (`mkimage.py`, `mkkernel.py`, `mkinitrd.py`), font generator, `ffstool` |
| `tests/` | host unit tests, end-to-end QEMU tests, static analysis script |

## Boot

The disk image (`tools/mkimage.py`) is a GPT disk with a protective MBR:

| Area | Purpose |
|---|---|
| LBA 0 | MBR with the BIOS stage 1 code |
| partition 1 (BIOS boot, 16 MiB) | stage 2, `KERNEL.BIN`, initrd |
| partition 2 (EFI system, FAT, 48 MiB) | `EFI/BOOT/BOOTX64.EFI`, `FRESH/KERNEL.BIN`, `FRESH/INITRD` |
| partition 3 (FreshFS) | user data, mounted at `/data` |

**BIOS path.** Stage 1 loads 32 KiB of stage 2 with INT 13h extensions. Stage 2
checks for a 64-bit CPU, enables A20, reads the E820 memory map, loads the
kernel to 2 MiB and the initrd right after it (through unreal mode), picks a
32-bit VESA mode (preferring 1024x768), builds page tables (identity map of the
first 4 GiB plus the kernel at -2 GiB) and enters long mode.

**UEFI path.** `BOOTX64.EFI` reads the kernel and initrd from the EFI system
partition, picks a GOP mode, finds the ACPI tables, builds equivalent page
tables, exits boot services and jumps to the kernel.

Both pass the same `struct boot_info` (`include/fresh/bootinfo.h`): memory map,
framebuffer, kernel/initrd location, ACPI RSDP and the command line. The kernel
image format (`KERNEL.BIN`) is a 64-byte header plus a flat image, produced from
the ELF file by `tools/mkkernel.py`.

## Kernel

**Memory.** The kernel runs at `0xFFFFFFFF80000000`. All physical memory is
mapped at `0xFFFF800000000000` (the "direct map", 2 MiB pages, MMIO uncached,
framebuffer write-combining through the PAT). Physical pages are tracked in a
bitmap. The heap uses power-of-two size classes up to 2 KiB and whole pages
above that; every block has a header that catches double frees.

**Processes.** Up to 64 processes, each with its own lower-half address space
(4 KiB pages, user stack grows on demand up to 8 MiB, heap via `sbrk`), file
descriptor table (32 entries) and working directory. Programs are static ELF64
files linked at 4 MiB. There is no `fork`: `spawn` starts a program directly and
can remap the child's standard input/output/error, which is all a shell needs
for pipes and redirection. Process groups let Ctrl+C stop a whole pipeline.

**Scheduling.** Round robin with a 20 ms time slice driven by the PIT at
100 Hz. The kernel itself is not preemptible: a process is switched out when it
blocks, exits, or when the timer fires while it runs in user mode. This removes
almost all locking. SSE state is saved and restored on every switch.

**System calls** use the `syscall` instruction (see [syscalls.md](syscalls.md)).
Every user pointer is range checked and copied with fault-tolerant routines: a
bad pointer returns `-EFAULT` instead of crashing the kernel. CPU exceptions in
user mode kill only the offending process; in kernel mode they panic with a
register dump.

**Files.** The VFS resolves paths (`.`/`..`, mount points) and dispatches to
two file systems: a RAM file system for `/` (filled from the initrd at boot)
and FreshFS for `/data`. Open files can be regular files, directories, pipe
ends or the terminal.

**Terminal.** Input comes from the PS/2 keyboard and the serial port, output
goes to the screen console and the serial port. In cooked mode the TTY edits
lines and echoes; in raw mode (used by the shell's line editor and `edit`)
programs see every byte. Arrow keys arrive as ANSI escape sequences.

**Disks.** The AHCI driver uses polled DMA through a bounce buffer; the IDE
driver uses PIO with LBA48. GPT is parsed to find the FreshFS partition.
FreshFS block I/O goes through a 256-block write-through cache.

**Power.** `shutdown` and `reboot` first unmount FreshFS cleanly (marking it
clean), then use ACPI S5 (the `\_S5` package is read from the DSDT) or the ACPI
reset register, with keyboard-controller and triple-fault fallbacks.

## Graphical desktop

`/bin/desktop` (sources in `user/desktop/`) is an ordinary user program:

- `SYS_FB_ACQUIRE` maps the framebuffer into its address space (write-combining,
  marked as device memory so it is never freed) and makes the kernel deliver
  keyboard and mouse input to it as events (`SYS_INPUT_READ`). The text console
  keeps running off-screen and on the serial port, and is redrawn when the
  desktop releases the screen or exits (even if it crashes).
- It draws into a back buffer and copies frames to the screen; the mouse
  pointer is drawn directly on the screen so moving it is cheap.
- Each Terminal window owns a **pseudo-terminal** (`SYS_PTY_OPEN`): the shell
  runs on the slave side with the full line discipline (line editing, Ctrl+C
  for its own foreground job, raw mode for `edit`, its own window size), and
  the desktop reads the master side and renders it with a small terminal
  emulator (`term.c`, the same escape sequences as the console).
- Closing a terminal window hangs up its pseudo-terminal: the shell reads end
  of file and exits.

init starts the desktop after `/etc/rc` when the machine has a linear
framebuffer and `/etc/nogui` does not exist. Desktop actions are logged to
`/tmp/desktop.log` (used by the tests).

## User space

The C library is small: system call wrappers (`fresh.h`), `malloc`
(address-ordered free list with coalescing), buffered stdio, string and
conversion functions, wildcard matching and date conversion. `init` runs
`/etc/rc`, changes to `/data` and keeps a shell running on the console.
