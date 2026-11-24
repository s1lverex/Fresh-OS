/* memtest - user heap stress: random malloc/realloc/free with patterns */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fresh.h>

#define SLOTS 512

int main(int argc, char **argv)
{
    int rounds = argc > 1 ? atoi(argv[1]) : 20000;
    static unsigned char *ptr[SLOTS];
    static size_t len[SLOTS];
    srand(12345);
    int errors = 0;
    for (int r = 0; r < rounds; r++) {
        int s = rand() % SLOTS;
        if (ptr[s]) {
            for (size_t k = 0; k < len[s]; k += 97)
                if (ptr[s][k] != (unsigned char)(s ^ k))
                    errors++;
            if (rand() % 3 == 0) {
                size_t nl = (size_t)(rand() % 20000) + 1;
                unsigned char *q = realloc(ptr[s], nl);
                if (!q) {
                    errors++;
                    continue;
                }
                size_t keep = nl < len[s] ? nl : len[s];
                for (size_t k = 0; k < keep; k += 97)
                    if (q[k] != (unsigned char)(s ^ k))
                        errors++;
                ptr[s] = q;
                len[s] = nl;
                for (size_t k = 0; k < nl; k++)
                    q[k] = (unsigned char)(s ^ k);
            } else {
                free(ptr[s]);
                ptr[s] = NULL;
            }
        } else {
            size_t n = (size_t)(rand() % 3 == 0 ? rand() % 200000 : rand() % 300) + 1;
            ptr[s] = malloc(n);
            if (!ptr[s]) {
                errors++;
                continue;
            }
            len[s] = n;
            for (size_t k = 0; k < n; k++)
                ptr[s][k] = (unsigned char)(s ^ k);
        }
    }
    for (int s = 0; s < SLOTS; s++)
        free(ptr[s]);
    size_t left = malloc_in_use();
    printf("memtest: %d rounds, %d errors, %zu bytes still in use: %s\n", rounds, errors, left,
           errors == 0 && left == 0 ? "PASS" : "FAIL");
    return errors == 0 && left == 0 ? 0 : 1;
}
