/* tail - print the last lines of a file.  tail [-n N] [file] */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fresh.h>

int main(int argc, char **argv)
{
    long n = 10;
    int i = 1;
    if (i + 1 < argc && !strcmp(argv[i], "-n")) {
        n = atol(argv[i + 1]);
        i += 2;
    }
    int fd = 0;
    if (i < argc) {
        fd = open(argv[i], O_RDONLY);
        if (fd < 0) {
            fprintf(stderr, "tail: %s: %s\n", argv[i], strerror(fd));
            return 1;
        }
    }
    size_t cap = 65536, len = 0;
    char *data = malloc(cap);
    long r;
    while ((r = read(fd, data + len, cap - len)) > 0) {
        len += (size_t)r;
        if (len == cap) {
            cap *= 2;
            data = realloc(data, cap);
            if (!data) {
                fprintf(stderr, "tail: out of memory\n");
                return 1;
            }
        }
    }
    size_t start = len;
    long lines = 0;
    if (start && data[start - 1] == '\n')
        start--;
    while (start > 0) {
        if (data[start - 1] == '\n' && ++lines == n)
            break;
        start--;
    }
    if (n <= 0)
        start = len;
    write(1, data + start, len - start);
    return 0;
}
