# Fresh OS

Fresh OS is a small, fast operating system for 64-bit PCs, written from scratch.
It is not based on Linux, BSD, Windows, macOS, iOS or Android: the boot loaders,
kernel, file system, C library and every program are original code in this
repository.

```
fresh:/data$ sysinfo
Fresh OS 0.1.0
  CPU       : QEMU Virtual CPU version 2.5+
  Memory    : 254 MiB total, 251 MiB free
  Firmware  : UEFI
  Display   : 1024x768 framebuffer
  Disks     : 1
  /data     : FreshFS (persistent)
```

## What it does

- **Boots on BIOS and UEFI PCs** from one disk image (own MBR/stage-2 loader and
  own `BOOTX64.EFI`).
- **Kernel**: x86-64 long mode, paging with a higher-half kernel, preemptive
  multitasking of user processes, memory protection (user/kernel separation,
  no-execute data), pseudo-terminals, 42 system calls.
- **Drivers**: framebuffer text console (original 8x16 font, ANSI colours),
  VGA text fallback, PS/2 keyboard and mouse, serial port, AHCI (SATA) and IDE disks,
  PCI, ACPI power-off/reboot, real-time clock.
- **FreshFS**: an original on-disk file system with crash recovery: the kernel
  checks and repairs it automatically after a power cut. Files in `/data` are
  stored on disk; the rest of the tree lives in RAM.
- **Graphical desktop**: movable windows, a taskbar with start menu and clock,
  desktop icons, mouse and keyboard, and apps: Terminal (as many as you like,
  each running a real shell), Files, System Monitor and About. The text console
  is still there behind it (and on the serial port).
- **Shell** with line editing, history, Tab completion, pipes, redirection,
  `;` `&&` `||`, background jobs, variables, wildcards and scripts.
- **Programs**: `ls cat cp mv rm mkdir rmdir touch echo wc head tail grep hexdump
  seq stat ps kill free uptime date df sysinfo uname edit sleep sync clear help
  reboot shutdown`, plus a full-screen text editor (`edit`).
- **Lightweight**: kernel ~100 KB, all programs ~1.6 MB, boots in about a
  second and runs in about 5 MB of RAM. Minimum RAM: 64 MB.

## Quick start

Requirements (Debian/Ubuntu): `clang lld nasm make python3 mtools qemu-system-x86 ovmf`.

```bash
make            # builds build/fresh-os.img
```

```bash
make run        # boot in QEMU with BIOS firmware (serial console in the terminal)
```

```bash
make run-uefi   # boot in QEMU with UEFI firmware (OVMF)
```

Fresh OS starts into its desktop. Open more terminals from the start menu
("Fresh" button) or with Ctrl+Alt+T; Alt+Tab switches windows, Alt+F4 closes
one. "Exit to console" returns to the full-screen text console (type `desktop`
to come back). Type `help` in a terminal for the commands. Use **Shut down**
in the start menu (or `shutdown`) to power off safely. To boot to the text
console instead, create `/etc/nogui` in the initrd (`rootfs/etc/nogui`).

### Running on a real PC

Write `build/fresh-os.img` to a USB stick or disk (this erases it):

```bash
sudo dd if=build/fresh-os.img of=/dev/sdX bs=4M conv=fsync
```

Then boot the PC from it (legacy/CSM or UEFI; Secure Boot must be off).
Fresh OS runs entirely from memory once booted. For files in `/data` to be
saved, the image must be on an internal SATA or IDE disk (there is no USB
storage driver yet; booted from a USB stick, `/data` is kept in RAM).
See [docs/hardware.md](docs/hardware.md) for what hardware is supported and the
current limitations: in particular Fresh OS needs a PS/2 keyboard or firmware
"USB legacy" keyboard emulation, as it has no USB stack yet.

## Testing

```bash
make test       # everything below (about 15 minutes, incl. a 10-minute soak test)
```

- **Host unit tests** (AddressSanitizer + UBSan): FreshFS model-based random
  testing against an in-memory reference, a corruption fuzzer for the file
  system checker, ELF loader fuzzing, kernel heap, `printf` and string routines
  compared with the host C library.
- **Static analysis**: clang static analyzer over every source file must be clean.
- **End-to-end tests in QEMU**: BIOS and UEFI on two machine types (SATA and IDE
  disks), 64 MB to 8 GB of RAM, several CPU models, every command and shell
  feature, the editor, the PS/2 keyboard, the framebuffer (screenshots), process
  isolation (12 kinds of crashing programs), syscall fuzzing, leak checks over
  thousands of process launches, disk-full handling, persistence across reboots,
  repeated power cuts during heavy writes, a GCC-built image, and a soak test.

See [docs/testing.md](docs/testing.md).

## Documentation

- [docs/architecture.md](docs/architecture.md) - how the system fits together
- [docs/freshfs.md](docs/freshfs.md) - the FreshFS on-disk format
- [docs/syscalls.md](docs/syscalls.md) - the system call interface
- [docs/hardware.md](docs/hardware.md) - supported hardware and limitations
- [docs/testing.md](docs/testing.md) - test suites and how to run them

## License

MIT, see [LICENSE](LICENSE).
