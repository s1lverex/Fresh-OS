/* Fresh OS - kernel/user ABI: syscall numbers, error codes, shared structs.
 * Calling convention: `syscall` with RAX = number, args in RDI RSI RDX R10
 * R8 R9, result in RAX (negative values are -errno). */
#ifndef FRESH_ABI_H
#define FRESH_ABI_H

#include <stdint.h>

#define FRESH_VERSION "0.1.0"

/* syscall numbers */
#define SYS_EXIT 0
#define SYS_SPAWN 1
#define SYS_WAIT 2
#define SYS_GETPID 3
#define SYS_KILL 4
#define SYS_SLEEP 5
#define SYS_UPTIME 6
#define SYS_OPEN 7
#define SYS_CLOSE 8
#define SYS_READ 9
#define SYS_WRITE 10
#define SYS_SEEK 11
#define SYS_STAT 12
#define SYS_READDIR 13
#define SYS_MKDIR 14
#define SYS_UNLINK 15
#define SYS_RENAME 16
#define SYS_PIPE 17
#define SYS_SBRK 18
#define SYS_SYSINFO 19
#define SYS_PROCINFO 20
#define SYS_REBOOT 21
#define SYS_POWEROFF 22
#define SYS_CHDIR 23
#define SYS_GETCWD 24
#define SYS_TIME 25
#define SYS_FSTAT 26
#define SYS_TTYMODE 27
#define SYS_TTYFG 28
#define SYS_TTYSIZE 29
#define SYS_STATFS 30
#define SYS_SYNC 31
#define SYS_DUP 32
#define SYS_GETPPID 33
#define SYS_TRUNCATE 34
#define SYS_YIELD 35
#define SYS_TEST 36 /* kernel diagnostics hooks used by the test suite */
#define SYS_FB_ACQUIRE 37 /* take over the screen and input devices (GUI) */
#define SYS_FB_RELEASE 38
#define SYS_INPUT_READ 39
#define SYS_PTY_OPEN 40
#define SYS_PTY_RESIZE 41
#define SYS_COUNT 42

/* error codes (returned negated) */
#define EPERM 1
#define ENOENT 2
#define ESRCH 3
#define EINTR 4
#define EIO 5
#define E2BIG 7
#define ENOEXEC 8
#define EBADF 9
#define ECHILD 10
#define EAGAIN 11
#define ENOMEM 12
#define EACCES 13
#define EFAULT 14
#define EBUSY 16
#define EEXIST 17
#define EXDEV 18
#define ENODEV 19
#define ENOTDIR 20
#define EISDIR 21
#define EINVAL 22
#define ENFILE 23
#define EMFILE 24
#define ENOTTY 25
#define EFBIG 27
#define ENOSPC 28
#define ESPIPE 29
#define EROFS 30
#define EPIPE 32
#define ERANGE 34
#define ENAMETOOLONG 36
#define ENOSYS 38
#define ENOTEMPTY 39

/* open flags */
#define O_RDONLY 0x0
#define O_WRONLY 0x1
#define O_RDWR 0x2
#define O_ACCMODE 0x3
#define O_CREAT 0x40
#define O_EXCL 0x80
#define O_TRUNC 0x200
#define O_APPEND 0x400

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

/* file types */
#define FT_FILE 1
#define FT_DIR 2
#define FT_DEV 3
#define FT_PIPE 4

#define NAME_MAX 57
#define PATH_MAX 256
#define MAX_ARGS 32
#define MAX_ARG_BYTES 4096

struct fresh_stat {
    uint32_t type;
    uint32_t mode;
    uint64_t size;
    uint64_t ino;
    uint64_t mtime;
    uint64_t ctime;
};

struct fresh_dirent {
    uint64_t ino;
    uint32_t type;
    char name[NAME_MAX + 3];
};

struct fresh_sysinfo {
    uint64_t mem_total;   /* bytes */
    uint64_t mem_free;    /* bytes */
    uint64_t uptime_ms;
    uint32_t nprocs;
    uint32_t boot_type;   /* 1 BIOS, 2 UEFI */
    uint32_t fb_width, fb_height;
    uint32_t ndisks;
    uint32_t data_mounted;
    char version[16];
    char cpu[48];
};

#define PROC_STATE_RUNNABLE 1
#define PROC_STATE_RUNNING 2
#define PROC_STATE_BLOCKED 3
#define PROC_STATE_ZOMBIE 4

struct fresh_procinfo {
    int32_t pid;
    int32_t ppid;
    uint32_t state;
    uint32_t pad;
    uint64_t mem_bytes;
    uint64_t cpu_ms;
    char name[32];
};

struct fresh_statfs {
    uint64_t block_size;
    uint64_t total_blocks;
    uint64_t free_blocks;
    uint64_t total_inodes;
    uint64_t free_inodes;
    char fs_name[16];
};

/* spawn: fd_map[i] = parent fd that becomes child fd i (0..2), -1 = same.
 * pgid: 0 = inherit the parent's process group, -1 = start a new group
 * led by the child, > 0 = join that group. Ctrl+C on the terminal kills the
 * foreground process group (see SYS_TTYFG). */
struct fresh_spawn {
    const char *path;
    char *const *argv;
    int32_t fd_map[3];
    int32_t pgid;
};

#define WAIT_NOHANG 1 /* SYS_WAIT option: return 0 if no child has exited */

#define TTY_MODE_COOKED 0
#define TTY_MODE_RAW 1

/* SYS_FB_ACQUIRE: the framebuffer is mapped into the caller at addr; pixels
 * are 32-bit with the given bit positions for red, green and blue */
struct fresh_fbinfo {
    uint64_t addr;
    uint32_t width, height, pitch, bpp;
    uint8_t red_pos, green_pos, blue_pos, pad;
    uint32_t reserved;
};

/* SYS_INPUT_READ(events, max, timeout_ms) returns the number of events */
#define INPUT_KEY 1
#define INPUT_MOUSE 2
struct fresh_input_event {
    uint32_t type;
    uint32_t key;     /* KEY_* or the ASCII code */
    uint32_t ch;      /* ASCII character (0 if none) */
    uint32_t mods;    /* MOD_* */
    int32_t dx, dy;   /* mouse motion, y grows downwards */
    uint32_t buttons; /* MOUSE_* currently pressed */
    uint32_t reserved;
};
#define KEY_UP 0x101
#define KEY_DOWN 0x102
#define KEY_LEFT 0x103
#define KEY_RIGHT 0x104
#define KEY_HOME 0x105
#define KEY_END 0x106
#define KEY_PGUP 0x107
#define KEY_PGDN 0x108
#define KEY_INSERT 0x109
#define KEY_DELETE 0x10A
#define KEY_F1 0x111 /* .. KEY_F1 + 11 */
#define MOD_SHIFT 1
#define MOD_CTRL 2
#define MOD_ALT 4
#define MOUSE_LEFT 1
#define MOUSE_RIGHT 2
#define MOUSE_MIDDLE 4

/* SYS_TEST operations */
#define TEST_FREE_PAGES 1  /* free physical pages (incl. heap-cached ones) */
#define TEST_HEAP_USED 2   /* returns bytes allocated in the kernel heap */
#define TEST_SELFTEST 3    /* run in-kernel self tests, returns failures */
#define TEST_PANIC 4       /* trigger a kernel panic (only in test mode) */

#endif
