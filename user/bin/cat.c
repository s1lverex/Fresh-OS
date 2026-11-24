/* cat - concatenate files to standard output */
#include <stdio.h>
#include <string.h>
#include <fresh.h>

static char buf[65536];

static int copy_fd(int fd, const char *name)
{
    long n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        long off = 0;
        while (off < n) {
            long w = write(1, buf + off, (size_t)(n - off));
            if (w <= 0)
                return 1;
            off += w;
        }
    }
    if (n < 0) {
        fprintf(stderr, "cat: %s: %s\n", name, strerror((int)n));
        return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    int rc = 0;
    if (argc < 2)
        return copy_fd(0, "stdin");
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-")) {
            rc |= copy_fd(0, "stdin");
            continue;
        }
        int fd = open(argv[i], O_RDONLY);
        if (fd < 0) {
            fprintf(stderr, "cat: %s: %s\n", argv[i], strerror(fd));
            rc = 1;
            continue;
        }
        rc |= copy_fd(fd, argv[i]);
        close(fd);
    }
    return rc;
}
