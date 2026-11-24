/* echo - print arguments */
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    int i = 1, newline = 1;
    if (argc > 1 && !strcmp(argv[1], "-n")) {
        newline = 0;
        i = 2;
    }
    for (; i < argc; i++) {
        fputs(argv[i], stdout);
        if (i + 1 < argc)
            putchar(' ');
    }
    if (newline)
        putchar('\n');
    return 0;
}
