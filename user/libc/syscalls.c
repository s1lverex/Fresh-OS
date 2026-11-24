/* Fresh OS libc - system call wrappers. */
#include <fresh.h>

long fresh_syscall(long n, long a, long b, long c, long d, long e, long f)
{
    register long r10 __asm__("r10") = d;
    register long r8 __asm__("r8") = e;
    register long r9 __asm__("r9") = f;
    long ret;
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9)
                     : "rcx", "r11", "memory");
    return ret;
}

#define S0(n) fresh_syscall(n, 0, 0, 0, 0, 0, 0)
#define S1(n, a) fresh_syscall(n, (long)(a), 0, 0, 0, 0, 0)
#define S2(n, a, b) fresh_syscall(n, (long)(a), (long)(b), 0, 0, 0, 0)
#define S3(n, a, b, c) fresh_syscall(n, (long)(a), (long)(b), (long)(c), 0, 0, 0)

void sys_exit(int code)
{
    S1(SYS_EXIT, code);
    for (;;)
        ;
}

int spawn(const char *path, char *const argv[], const int fd_map[3], int pgid)
{
    struct fresh_spawn s;
    s.path = path;
    s.argv = argv;
    for (int i = 0; i < 3; i++)
        s.fd_map[i] = fd_map ? fd_map[i] : -1;
    s.pgid = pgid;
    return (int)S1(SYS_SPAWN, &s);
}

int wait_pid(int pid, int *status) { return (int)S3(SYS_WAIT, pid, status, 0); }
int wait_pid_opt(int pid, int *status, int options) { return (int)S3(SYS_WAIT, pid, status, options); }
int getpid(void) { return (int)S0(SYS_GETPID); }
int getppid(void) { return (int)S0(SYS_GETPPID); }
int kill(int pid) { return (int)S1(SYS_KILL, pid); }
int sleep_ms(uint64_t ms) { return (int)S1(SYS_SLEEP, ms); }
uint64_t uptime_ms(void) { return (uint64_t)S0(SYS_UPTIME); }
int open(const char *path, int flags) { return (int)S2(SYS_OPEN, path, flags); }
int close(int fd) { return (int)S1(SYS_CLOSE, fd); }
long read(int fd, void *buf, size_t n) { return S3(SYS_READ, fd, buf, n); }
long write(int fd, const void *buf, size_t n) { return S3(SYS_WRITE, fd, buf, n); }
long lseek(int fd, long off, int whence) { return S3(SYS_SEEK, fd, off, whence); }
int stat(const char *path, struct fresh_stat *st) { return (int)S2(SYS_STAT, path, st); }
int fstat(int fd, struct fresh_stat *st) { return (int)S2(SYS_FSTAT, fd, st); }
int readdir(int fd, struct fresh_dirent *ent) { return (int)S2(SYS_READDIR, fd, ent); }
int mkdir(const char *path) { return (int)S1(SYS_MKDIR, path); }
int unlink(const char *path) { return (int)S1(SYS_UNLINK, path); }
int rename(const char *oldp, const char *newp) { return (int)S2(SYS_RENAME, oldp, newp); }
int pipe(int fds[2]) { return (int)S1(SYS_PIPE, fds); }
void *sbrk(long incr) { return (void *)S1(SYS_SBRK, incr); }
int sysinfo(struct fresh_sysinfo *si) { return (int)S1(SYS_SYSINFO, si); }
int procinfo(int index, struct fresh_procinfo *pi) { return (int)S2(SYS_PROCINFO, index, pi); }
int reboot(void) { return (int)S0(SYS_REBOOT); }
int poweroff(void) { return (int)S0(SYS_POWEROFF); }
int chdir(const char *path) { return (int)S1(SYS_CHDIR, path); }
int getcwd(char *buf, size_t n) { return (int)S2(SYS_GETCWD, buf, n); }
uint64_t unix_time(void) { return (uint64_t)S0(SYS_TIME); }
int ttymode(int mode) { return (int)S1(SYS_TTYMODE, mode); }
int ttyfg(int pgid) { return (int)S1(SYS_TTYFG, pgid); }
int statfs(const char *path, struct fresh_statfs *st) { return (int)S2(SYS_STATFS, path, st); }
int sync(void) { return (int)S0(SYS_SYNC); }
int dup(int fd) { return (int)S1(SYS_DUP, fd); }
int truncate(const char *path, uint64_t size) { return (int)S2(SYS_TRUNCATE, path, size); }
int yield(void) { return (int)S0(SYS_YIELD); }
long fresh_test(int op, long arg) { return S2(SYS_TEST, op, arg); }

int ttysize(int *rows, int *cols)
{
    long v = S0(SYS_TTYSIZE);
    if (rows)
        *rows = (int)(v >> 16);
    if (cols)
        *cols = (int)(v & 0xFFFF);
    return 0;
}

int isatty(int fd)
{
    struct fresh_stat st = {0, 0, 0, 0, 0, 0};
    return fstat(fd, &st) == 0 && st.type == FT_DEV;
}
