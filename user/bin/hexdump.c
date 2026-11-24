/* hexdump - show file contents in hex and ASCII */
#include <stdio.h>
#include <fresh.h>

int main(int argc, char **argv)
{
    int fd = 0;
    if (argc > 1) {
        fd = open(argv[1], O_RDONLY);
        if (fd < 0) {
            fprintf(stderr, "hexdump: %s: %s\n", argv[1], strerror(fd));
            return 1;
        }
    }
    unsigned char line[16];
    unsigned long off = 0;
    for (;;) {
        long n = 0;
        while (n < 16) {
            long r = read(fd, line + n, (size_t)(16 - n));
            if (r <= 0)
                break;
            n += r;
        }
        if (n <= 0)
            break;
        printf("%08lx  ", off);
        for (int i = 0; i < 16; i++) {
            if (i < n)
                printf("%02x ", line[i]);
            else
                printf("   ");
            if (i == 7)
                putchar(' ');
        }
        printf(" |");
        for (int i = 0; i < n; i++)
            putchar(line[i] >= 32 && line[i] < 127 ? line[i] : '.');
        printf("|\n");
        off += (unsigned long)n;
        if (n < 16)
            break;
    }
    printf("%08lx\n", off);
    return 0;
}
