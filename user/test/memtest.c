/* memtest - user heap stress: random malloc/realloc/free with patterns */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fresh.h>

#define SLOTS 512

unsigned char *ptr[SLOTS]; /* global: allocations stay reachable */
size_t len[SLOTS];

int main(int argc, char **argv)
{
    int rounds = argc > 1 ? atoi(argv[1]) : 20000;
    srand(12345);
    int errors = 0;
    for (int r = 0; r < rounds; r++) {
        int s = rand() % SLOTS;
        unsigned char *cur = ptr[s];
        if (cur) {
            for (size_t k = 0; k < len[s]; k += 97)
                if (cur[k] != (unsigned char)(s ^ k))
                    errors++;
            if (rand() % 3 == 0) {
                size_t nl = (size_t)(rand() % 20000) + 1;
                unsigned char *q = realloc(cur, nl);
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
                free(cur);
                ptr[s] = NULL;
            }
        } else {
            size_t n = (size_t)(rand() % 3 == 0 ? rand() % 200000 : rand() % 300) + 1;
            unsigned char *q = malloc(n);
            if (!q) {
                errors++;
                continue;
            }
            for (size_t k = 0; k < n; k++)
                q[k] = (unsigned char)(s ^ k);
            ptr[s] = q;
            len[s] = n;
        }
    }
    for (int s = 0; s < SLOTS; s++)
        free(ptr[s]);
    size_t left = malloc_in_use();
    printf("memtest: %d rounds, %d errors, %zu bytes still in use: %s\n", rounds, errors, left,
           errors == 0 && left == 0 ? "PASS" : "FAIL");
    return errors == 0 && left == 0 ? 0 : 1;
}
