/* pipetest - stream data between processes through a pipe and verify it */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fresh.h>

int main(int argc, char **argv)
{
    long total = 4L << 20;
    if (argc > 1 && !strcmp(argv[1], "--writer")) {
        static unsigned char buf[8192];
        long done = 0;
        while (done < total) {
            long n = total - done < (long)sizeof(buf) ? total - done : (long)sizeof(buf);
            for (long i = 0; i < n; i++)
                buf[i] = (unsigned char)((done + i) * 31 + 7);
            long w = write(1, buf, (size_t)n);
            if (w <= 0)
                return 1;
            done += w;
        }
        return 0;
    }
    int fds[2];
    if (pipe(fds) < 0) {
        printf("pipetest: pipe failed\n");
        return 1;
    }
    char *args[] = {"pipetest", "--writer", NULL};
    int map[3] = {-1, fds[1], -1};
    int pid = spawn("/test/pipetest", args, map, 0);
    close(fds[1]);
    if (pid < 0) {
        printf("pipetest: spawn failed: %s\n", strerror(pid));
        return 1;
    }
    static unsigned char buf[5000];
    long got = 0, bad = 0, n;
    uint64_t t0 = uptime_ms();
    while ((n = read(fds[0], buf, sizeof(buf))) > 0) {
        for (long i = 0; i < n; i++)
            if (buf[i] != (unsigned char)((got + i) * 31 + 7))
                bad++;
        got += n;
    }
    close(fds[0]);
    int st;
    wait_pid(pid, &st);
    uint64_t ms = uptime_ms() - t0;
    int pass = got == total && bad == 0 && st == 0;
    printf("pipetest: %ld bytes in %llu ms, %ld corrupt, writer status %d: %s\n", got, (unsigned long long)ms, bad, st,
           pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}
