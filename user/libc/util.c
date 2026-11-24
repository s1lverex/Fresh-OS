/* Fresh OS libc - error strings, command spawning, wildcards, time. */
#include <fresh.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

const char *strerror(int err)
{
    if (err < 0)
        err = -err;
    switch (err) {
    case 0: return "success";
    case EPERM: return "operation not permitted";
    case ENOENT: return "no such file or directory";
    case ESRCH: return "no such process";
    case EINTR: return "interrupted";
    case EIO: return "input/output error";
    case E2BIG: return "argument list too long";
    case ENOEXEC: return "not an executable";
    case EBADF: return "bad file descriptor";
    case ECHILD: return "no child processes";
    case EAGAIN: return "resource temporarily unavailable";
    case ENOMEM: return "out of memory";
    case EACCES: return "permission denied";
    case EFAULT: return "bad address";
    case EBUSY: return "resource busy";
    case EEXIST: return "file exists";
    case EXDEV: return "cross-device link";
    case ENODEV: return "no such device";
    case ENOTDIR: return "not a directory";
    case EISDIR: return "is a directory";
    case EINVAL: return "invalid argument";
    case ENFILE: return "too many open files in system";
    case EMFILE: return "too many open files";
    case ENOTTY: return "not a terminal";
    case EFBIG: return "file too large";
    case ENOSPC: return "no space left on device";
    case ESPIPE: return "illegal seek";
    case EROFS: return "read-only file system";
    case EPIPE: return "broken pipe";
    case ERANGE: return "result out of range";
    case ENAMETOOLONG: return "file name too long";
    case ENOSYS: return "function not implemented";
    case ENOTEMPTY: return "directory not empty";
    default: return "unknown error";
    }
}

int spawn_cmd(const char *cmd, char *const argv[], const int fd_map[3], int pgid)
{
    char path[PATH_MAX];
    if (strchr(cmd, '/')) {
        strlcpy(path, cmd, sizeof(path));
    } else {
        snprintf(path, sizeof(path), "/bin/%s", cmd);
        struct fresh_stat st;
        if (stat(path, &st) != 0) {
            snprintf(path, sizeof(path), "/data/bin/%s", cmd);
            if (stat(path, &st) != 0)
                return -ENOENT;
        }
    }
    int pid = spawn(path, argv, fd_map, pgid);
    if (pid == -ENOEXEC) {
        /* run text files as shell scripts */
        char *args[MAX_ARGS + 2];
        int n = 0;
        args[n++] = "sh";
        args[n++] = path;
        for (int i = 1; argv && argv[i] && n < MAX_ARGS; i++)
            args[n++] = argv[i];
        args[n] = NULL;
        pid = spawn("/bin/sh", args, fd_map, pgid);
    }
    return pid;
}

int glob_match(const char *p, const char *s)
{
    while (*p) {
        if (*p == '*') {
            while (*p == '*')
                p++;
            if (!*p)
                return 1;
            for (; *s; s++)
                if (glob_match(p, s))
                    return 1;
            return 0;
        }
        if (!*s)
            return 0;
        if (*p == '?') {
            p++;
            s++;
            continue;
        }
        if (*p == '[') {
            const char *q = p + 1;
            int neg = *q == '!' || *q == '^';
            if (neg)
                q++;
            int ok = 0;
            while (*q && *q != ']') {
                if (q[1] == '-' && q[2] && q[2] != ']') {
                    if (*s >= q[0] && *s <= q[2])
                        ok = 1;
                    q += 3;
                } else {
                    if (*s == *q)
                        ok = 1;
                    q++;
                }
            }
            if (*q != ']') { /* no closing bracket: literal '[' */
                if (*s != '[')
                    return 0;
                p++;
                s++;
                continue;
            }
            if (ok == neg)
                return 0;
            p = q + 1;
            s++;
            continue;
        }
        if (*p != *s)
            return 0;
        p++;
        s++;
    }
    return *s == 0;
}

void unix_to_tm(uint64_t t, struct tm_simple *tm)
{
    uint64_t days = t / 86400, rem = t % 86400;
    tm->hour = (int)(rem / 3600);
    tm->min = (int)(rem % 3600 / 60);
    tm->sec = (int)(rem % 60);
    tm->wday = (int)((days + 4) % 7); /* 1970-01-01 was a Thursday */
    /* civil from days (Howard Hinnant's algorithm) */
    int64_t z = (int64_t)days + 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    uint64_t doe = (uint64_t)(z - era * 146097);
    uint64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t y = (int64_t)yoe + era * 400;
    uint64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    uint64_t mp = (5 * doy + 2) / 153;
    tm->day = (int)(doy - (153 * mp + 2) / 5 + 1);
    tm->month = (int)(mp < 10 ? mp + 3 : mp - 9);
    tm->year = (int)(y + (tm->month <= 2));
}
