/* Fresh OS system call interface for user programs.
 * All calls return a negative error code (-ENOENT, ...) on failure. */
#ifndef _FRESH_H
#define _FRESH_H

#include <stdint.h>
#include <stddef.h>
#include "fresh/abi.h"

long fresh_syscall(long n, long a, long b, long c, long d, long e, long f);

__attribute__((noreturn)) void sys_exit(int code);
int spawn(const char *path, char *const argv[], const int fd_map[3], int pgid);
int wait_pid(int pid, int *status);
int wait_pid_opt(int pid, int *status, int options);
int getpid(void);
int getppid(void);
int kill(int pid);
int sleep_ms(uint64_t ms);
uint64_t uptime_ms(void);
int open(const char *path, int flags);
int close(int fd);
long read(int fd, void *buf, size_t n);
long write(int fd, const void *buf, size_t n);
long lseek(int fd, long off, int whence);
int stat(const char *path, struct fresh_stat *st);
int fstat(int fd, struct fresh_stat *st);
int readdir(int fd, struct fresh_dirent *ent);
int mkdir(const char *path);
int unlink(const char *path);
int rename(const char *oldp, const char *newp);
int pipe(int fds[2]);
void *sbrk(long incr);
int sysinfo(struct fresh_sysinfo *si);
int procinfo(int index, struct fresh_procinfo *pi);
int reboot(void);
int poweroff(void);
int chdir(const char *path);
int getcwd(char *buf, size_t n);
uint64_t unix_time(void);
int ttymode(int mode); /* returns previous mode; -1 only queries */
int ttyfg(int pgid);
int ttysize(int *rows, int *cols);
int statfs(const char *path, struct fresh_statfs *st);
int sync(void);
int dup(int fd);
int truncate(const char *path, uint64_t size);
int yield(void);
long fresh_test(int op, long arg);
int fb_acquire(struct fresh_fbinfo *info); /* take over the screen + input */
int fb_release(void);
int input_read(struct fresh_input_event *ev, int max, int timeout_ms);
int pty_open(int fds[2]); /* fds[0] master (non-blocking reads), fds[1] slave */
int pty_resize(int master, int rows, int cols);

/* helpers */
const char *strerror(int err); /* accepts positive or negative codes */
int isatty(int fd);
/* spawn searching /bin when path has no '/'; runs text files with /bin/sh */
int spawn_cmd(const char *cmd, char *const argv[], const int fd_map[3], int pgid);
/* shell-style wildcard match: * ? [abc] */
int glob_match(const char *pat, const char *s);

struct tm_simple {
    int year, month, day, hour, min, sec, wday;
};
void unix_to_tm(uint64_t t, struct tm_simple *tm);

#endif
