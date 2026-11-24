# Testing Fresh OS

```bash
make test                 # full gate: host tests, static analysis, all QEMU tests
make test SOAK=60         # same with a 1-minute soak instead of 10 minutes
make host-test            # host unit tests only
make analyze              # clang static analyzer only
tests/gate.sh 3           # release gate: the full suite 3 times in a row, no failures allowed
python3 tests/e2e/run_all.py --list            # list end-to-end tests
python3 tests/e2e/run_all.py --only editor     # run selected tests
```

Set `FRESH_KEEP=1` to keep each VM's directory (`/tmp/freshvm-*`) with its
serial log and disk image.

## Host unit tests (`tests/host`, AddressSanitizer + UBSan)

| Test | What it checks |
|---|---|
| `test_freshfs` | thousands of random create/write/truncate/unlink/rename operations on a RAM disk compared against an in-memory model; periodic remount + fsck; tiny disks to hit "disk full"; bad names; I/O errors |
| `test_fsck_fuzz` | 3000 rounds of random corruption of a populated file system: the file system code must not crash on damaged data, `fsck -y` must always produce a clean, mountable, writable file system |
| `test_kernel_units` | kernel heap stress and accounting; 20000 mutated ELF files fed to the program loader |
| `test_fmt` | `printf` formatting and string routines compared with the host C library |

## In-OS tests (`user/test`, installed in `/test`)

| Program | What it checks |
|---|---|
| `ktest` | in-kernel self tests: formatting, page allocator, heap, paths, pipes, ramfs and FreshFS operations, steady-state leak checks |
| `crashtest MODE` | misbehaving programs (null pointer, kernel memory access, NX violation, privileged instructions, port I/O, divide error, invalid opcode, stack overflow, non-canonical address, bad system call pointers, huge allocations, CPU hog, out of memory) |
| `spawntest N` | N process launches; free page and heap counters must not change; 40 concurrent children |
| `memtest` | random malloc/realloc/free with data verification |
| `pipetest` | 4 MiB through a pipe between two processes, verified |
| `fdtest` | descriptor limit, `dup`, error codes |
| `fstest DIR [--fill]` | many files, large files (double indirect), rename/delete, sparse files, error cases, optional disk-full, free space restored |
| `sysfuzz N SEED` | N system calls with random arguments; the system must stay healthy |

## End-to-end tests (`tests/e2e/run_all.py`)

Boot matrix (BIOS/UEFI x i440FX-IDE/Q35-SATA, 64 MB-8 GB, CPU models), kernel
self tests, kernel panic reporting, process isolation, syscall fuzzing, stress,
disk full, persistence across reboots, repeated power cuts during heavy
writes (with host-side `fsck` afterwards), file operations, shell features,
line editing and Tab completion, all utilities, the editor, the PS/2 keyboard
(QEMU key injection), the framebuffer (QEMU screenshots), reboot, a GCC-built
image and a soak test.
