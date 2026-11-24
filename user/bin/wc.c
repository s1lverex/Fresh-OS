/* wc - count lines, words and bytes.  wc [-l] [-w] [-c] [file...] */
#include <stdio.h>
#include <ctype.h>
#include <string.h>
#include <fresh.h>

static char buf[65536];
static int show_l, show_w, show_c;

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

static void print(unsigned long long l, unsigned long long w, unsigned long long c, const char *name)
{
    if (show_l)
        printf("%7llu ", l);
    if (show_w)
        printf("%7llu ", w);
    if (show_c)
        printf("%7llu ", c);
    printf("%s\n", name);
}

int main(int argc, char **argv)
{
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        for (char *p = argv[i] + 1; *p; p++) {
            if (*p == 'l')
                show_l = 1;
            else if (*p == 'w')
                show_w = 1;
            else if (*p == 'c')
                show_c = 1;
            else {
                fprintf(stderr, "usage: wc [-lwc] [file...]\n");
                return 2;
            }
        }
    }
    if (!show_l && !show_w && !show_c)
        show_l = show_w = show_c = 1;
    unsigned long long tl = 0, tw = 0, tc = 0;
    int rc = 0, files = argc - i;
    if (files == 0) {
        unsigned long long l = 0, w = 0, c = 0;
        count(0, &l, &w, &c);
        print(l, w, c, "");
        return 0;
    }
    for (; i < argc; i++) {
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
        print(l, w, c, argv[i]);
        tl += l;
        tw += w;
        tc += c;
    }
    if (files > 1)
        print(tl, tw, tc, "total");
    return rc;
}
