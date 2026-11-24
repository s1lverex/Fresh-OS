/* fdtest - file descriptor limits, dup, closing, EOF behaviour */
#include <stdio.h>
#include <string.h>
#include <fresh.h>

int main(void)
{
    int fds[64], n = 0, bad = 0;
    for (;;) {
        int fd = open("/etc/motd", O_RDONLY);
        if (fd < 0) {
            if (fd != -EMFILE) {
                printf("fdtest: unexpected error %d\n", fd);
                bad = 1;
            }
            break;
        }
        if (n >= 64) {
            printf("fdtest: no descriptor limit\n");
            bad = 1;
            break;
        }
        fds[n++] = fd;
    }
    for (int i = 0; i < n; i++)
        if (close(fds[i]) != 0)
            bad = 1;
    if (close(fds[0]) != -EBADF)
        bad = 1;
    int fd = open("/etc/motd", O_RDONLY);
    int d = dup(fd);
    char a[4], b[4];
    if (read(fd, a, 4) != 4 || read(d, b, 4) != 4 || !memcmp(a, b, 4)) {
        printf("fdtest: dup does not share the offset\n");
        bad = 1;
    }
    close(fd);
    close(d);
    if (write(open("/etc/motd", O_RDONLY), "x", 1) != -EBADF)
        bad = 1;
    if (open("/", O_WRONLY) != -EISDIR)
        bad = 1;
    if (open("/no/such/file", O_RDONLY) != -ENOENT)
        bad = 1;
    printf("fdtest: %d descriptors max: %s\n", n + 3, bad ? "FAIL" : "PASS");
    return bad;
}
