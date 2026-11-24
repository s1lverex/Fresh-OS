/* wc - count lines, words and bytes */
#include <stdio.h>
#include <ctype.h>
#include <string.h>
#include <fresh.h>

static char buf[65536];

static int count(int fd, unsigned long long *l, unsigned long long *w, unsigned long long *c)
{
    long n;
    int inword = 0;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        for (long i = 0; i < n; i++) {
            unsigned char ch = (unsigned char)buf[i];
            if (ch == '\n')
                (*l)++;
            if (isspace(ch))
                inword = 0;
            else if (!inword) {
                inword = 1;
                (*w)++;
            }
        }
        *c += (unsigned long long)n;
    }
    return n < 0 ? (int)n : 0;
}

int main(int argc, char **argv)
{
    unsigned long long tl = 0, tw = 0, tc = 0;
    int rc = 0;
    if (argc < 2) {
        unsigned long long l = 0, w = 0, c = 0;
        count(0, &l, &w, &c);
        printf("%7llu %7llu %7llu\n", l, w, c);
        return 0;
    }
    for (int i = 1; i < argc; i++) {
        int fd = open(argv[i], O_RDONLY);
        if (fd < 0) {
            fprintf(stderr, "wc: %s: %s\n", argv[i], strerror(fd));
            rc = 1;
            continue;
        }
        unsigned long long l = 0, w = 0, c = 0;
        int r = count(fd, &l, &w, &c);
        close(fd);
        if (r < 0) {
            fprintf(stderr, "wc: %s: %s\n", argv[i], strerror(r));
            rc = 1;
            continue;
        }
        printf("%7llu %7llu %7llu %s\n", l, w, c, argv[i]);
        tl += l;
        tw += w;
        tc += c;
    }
    if (argc > 2)
        printf("%7llu %7llu %7llu total\n", tl, tw, tc);
    return rc;
}
