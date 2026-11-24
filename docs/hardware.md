# Hardware support

## Requirements

- 64-bit x86 CPU (any Intel or AMD processor from roughly 2005 onwards) with SSE2
- 64 MB of RAM or more (tested up to 8 GB)
- BIOS (legacy/CSM) or UEFI firmware; Secure Boot must be disabled
- a disk or USB stick the firmware can boot from

## Supported devices

| Area | Supported |
|---|---|
| Firmware | BIOS with INT 13h extensions; UEFI 2.x (x86-64) |
| Display | linear framebuffer from VESA (BIOS) or GOP (UEFI), 32 bits per pixel; VGA text mode fallback |
| Keyboard | PS/2 keyboard (US layout), or a USB keyboard through the firmware's PS/2 emulation |
| Serial | 16550 UART on COM1 (115200 baud), used as a second console |
| Storage | AHCI SATA controllers, IDE/PATA controllers (legacy or native mode) |
| Partitions | GPT; FreshFS data partition |
| Power | ACPI power off (S5) and reset; keyboard-controller reset fallback |
| Clock | CMOS real-time clock, PIT timer |
| Interrupts | 8259 PIC |

## Verification status

All of the above is exercised by the automated test suite in QEMU (i440FX and
Q35 machine types, SeaBIOS and OVMF firmware, SATA and IDE disks, several CPU
models, 64 MB to 8 GB). **Fresh OS 0.1 has not yet been tested on physical
PCs.** Real firmware and chipsets vary; please report problems.

## Known limitations (v0.1)

- **No USB stack.** USB keyboards only work if the firmware emulates a PS/2
  keyboard. Many PCs do this when booting in legacy (BIOS/CSM) mode; with UEFI
  boot the emulation often stops once the operating system takes over, so a
  PS/2 keyboard (or the serial console) may be required.
- **No NVMe driver.** Fresh OS can boot from an NVMe drive through the firmware,
  but `/data` is only persistent on SATA/IDE disks (USB sticks are read by the
  firmware at boot, so `/data` is not available when booting from USB).
  Without a supported disk, `/data` falls back to RAM and a message says so.
- Single CPU core is used; no networking, sound or graphical desktop.
- The 8259 PIC is used for interrupts (no APIC), which every PC chipset still
  provides.
- US keyboard layout only; text console only (8x16 font, ASCII).
- FreshFS has no journal: after a power cut it is repaired by a full check at
  the next boot, and data written in the last moments before the cut may be lost
  (files that were saved and synced, e.g. by `edit`, are safe).
