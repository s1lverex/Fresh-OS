# Fresh OS system calls

Programs call the kernel with the `syscall` instruction: number in `RAX`,
arguments in `RDI RSI RDX R10 R8 R9`, result in `RAX`. Negative results are
error codes (`-ENOENT`, ...; values in `include/fresh/abi.h`). The C wrappers in
`user/libc/include/fresh.h` return those values directly.

| # | Name | Arguments | Result |
|---|---|---|---|
| 0 | exit | code | does not return |
| 1 | spawn | `struct fresh_spawn *` (path, argv, fd_map[3], pgid) | child pid |
| 2 | wait | pid (-1 = any), int *status, options (`WAIT_NOHANG`) | pid, 0, or -ECHILD |
| 3 | getpid | | pid |
| 4 | kill | pid | 0 (init cannot be killed) |
| 5 | sleep | milliseconds | 0 or -EINTR |
| 6 | uptime | | milliseconds since boot |
| 7 | open | path, flags (`O_RDONLY O_WRONLY O_RDWR O_CREAT O_EXCL O_TRUNC O_APPEND`) | fd |
| 8 | close | fd | 0 |
| 9 | read | fd, buffer, count | bytes read (0 = end of file) |
| 10 | write | fd, buffer, count | bytes written |
| 11 | seek | fd, offset, whence | new offset |
| 12 | stat | path, `struct fresh_stat *` | 0 |
| 13 | readdir | fd, `struct fresh_dirent *` | 1 = entry, 0 = end |
| 14 | mkdir | path | 0 |
| 15 | unlink | path (files and empty directories) | 0 |
| 16 | rename | old path, new path | 0 |
| 17 | pipe | int fds[2] | 0 |
| 18 | sbrk | increment | previous break |
| 19 | sysinfo | `struct fresh_sysinfo *` | 0 |
| 20 | procinfo | index, `struct fresh_procinfo *` | 1 = filled, 0 = no more |
| 21 | reboot | | does not return |
| 22 | poweroff | | does not return |
| 23 | chdir | path | 0 |
| 24 | getcwd | buffer, size | length |
| 25 | time | | Unix time (seconds, UTC) |
| 26 | fstat | fd, `struct fresh_stat *` | 0 |
| 27 | ttymode | 0 cooked / 1 raw / -1 query | previous mode |
| 28 | ttyfg | process group for Ctrl+C (0 = none) | 0 |
| 29 | ttysize | | rows << 16 \| columns |
| 30 | statfs | path, `struct fresh_statfs *` | 0 |
| 31 | sync | | 0 |
| 32 | dup | fd | new fd sharing the open file |
| 33 | getppid | | parent pid |
| 34 | truncate | path, size | 0 |
| 35 | yield | | 0 |
| 36 | test | operation, argument | diagnostics used by the test suite |

## Exit status

`wait` reports `0..255` for a normal exit, `0x100` when the process was killed
(`kill`, Ctrl+C) and `0x200 | vector` when a CPU exception killed it. The shell
shows these as 130 and 139.

## Process start

A new process gets `argc` in `RDI` and `argv` in `RSI`; the strings live at the
top of its stack. File descriptors 0-2 come from the parent (or from
`fd_map`); all others start closed.
