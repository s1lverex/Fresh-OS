/* head - print the first lines of files.  head [-n N] [file...] */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fresh.h>

static void head(FILE *f, long n)
{
    int c;
    while (n > 0 && (c = fgetc(f)) != EOF) {
        putchar(c);
        if (c == '\n')
            n--;
    }
}

int main(int argc, char **argv)
{
    long n = 10;
    int i = 1, rc = 0;
    if (i + 1 < argc && !strcmp(argv[i], "-n")) {
        n = atol(argv[i + 1]);
        i += 2;
    }
    if (i >= argc) {
        head(stdin, n);
        return 0;
    }
    for (; i < argc; i++) {
        FILE *f = fopen(argv[i], "r");
        if (!f) {
            fprintf(stderr, "head: %s: cannot open\n", argv[i]);
            rc = 1;
            continue;
        }
        head(f, n);
        fclose(f);
    }
    return rc;
}
