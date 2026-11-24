/* sleep - pause for N seconds (fractions allowed, e.g. 0.5) */
#include <stdio.h>
#include <stdlib.h>
#include <fresh.h>

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: sleep seconds\n");
        return 2;
    }
    char *end;
    long whole = strtol(argv[1], &end, 10);
    long ms = whole * 1000;
    if (*end == '.') {
        long scale = 100;
        for (char *p = end + 1; *p >= '0' && *p <= '9' && scale; p++, scale /= 10)
            ms += (*p - '0') * scale;
    }
    return sleep_ms((uint64_t)(ms < 0 ? 0 : ms)) < 0 ? 1 : 0;
}
