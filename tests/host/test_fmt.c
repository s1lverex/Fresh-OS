/* Compare Fresh OS vsnprintf and string routines against the host C library. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>

int fresh_snprintf(char *buf, size_t size, const char *fmt, ...);
void *f_memmove(void *d, const void *s, size_t n);
void *f_memcpy(void *d, const void *s, size_t n);
void *f_memset(void *d, int c, size_t n);
int f_memcmp(const void *a, const void *b, size_t n);
size_t f_strlen(const char *s);
int f_strcmp(const char *a, const char *b);
int f_strncmp(const char *a, const char *b, size_t n);
char *f_strchr(const char *s, int c);
char *f_strrchr(const char *s, int c);
char *f_strstr(const char *h, const char *n);
size_t f_strlcpy(char *d, const char *s, size_t n);

static int failures;

#define CMP(fmt, ...)                                                                                        \
    do {                                                                                                    \
        char a[256], b[256];                                                                                \
        int ra = snprintf(a, sizeof(a), fmt, __VA_ARGS__);                                                  \
        int rb = fresh_snprintf(b, sizeof(b), fmt, __VA_ARGS__);                                            \
        if (ra != rb || strcmp(a, b)) {                                                                     \
            printf("FAIL format %-12s libc=[%s](%d) fresh=[%s](%d)\n", fmt, a, ra, b, rb);                  \
            failures++;                                                                                     \
        }                                                                                                   \
    } while (0)

int main(void)
{
    long long vals[] = {0, 1, -1, 42, -42, 123456789, -987654321, 2147483647LL, -2147483648LL,
                        9223372036854775807LL, (-9223372036854775807LL - 1)};
    for (size_t i = 0; i < sizeof(vals) / sizeof(vals[0]); i++) {
        long long v = vals[i];
        CMP("%lld", v);
        CMP("%20lld|", v);
        CMP("%-20lld|", v);
        CMP("%020lld", v);
        CMP("%+lld", v);
        CMP("% lld", v);
        CMP("%.5lld", v);
        CMP("%llx", (unsigned long long)v);
        CMP("%#llx", (unsigned long long)v);
        CMP("%llX", (unsigned long long)v);
        CMP("%llo", (unsigned long long)v);
        CMP("%llu", (unsigned long long)v);
        CMP("%d", (int)v);
        CMP("%5d", (int)v);
        CMP("%u", (unsigned)v);
        CMP("%x", (unsigned)v);
        CMP("%08x", (unsigned)v);
        CMP("%hd", (short)v);
        CMP("%hhu", (unsigned char)v);
        CMP("%zu", (size_t)v);
    }
    CMP("%s", "hello");
    CMP("%10s|", "hi");
    CMP("%-10s|", "hi");
    CMP("%.3s", "abcdef");
    CMP("%*d", 6, 42);
    CMP("%-*d|", 6, 42);
    CMP("%.*s", 2, "xyz");
    CMP("%c%c%c", 'a', 'b', 'c');
    CMP("%5c|", 'z');
    CMP("%%%d%%", 5);
    CMP("%p", (void *)0x1234);
    CMP("%s=%d (%x)", "key", 255, 255);
    /* truncation */
    char t[8];
    int r = fresh_snprintf(t, sizeof(t), "%s", "0123456789");
    if (r != 10 || strcmp(t, "0123456")) {
        printf("FAIL truncation\n");
        failures++;
    }
    /* string routines on random data */
    uint64_t s = 7;
    for (int it = 0; it < 100000; it++) {
        char a[300], b[300], c[300];
        s = s * 6364136223846793005ULL + 1;
        size_t n = (size_t)(s >> 50) % 256;
        for (size_t k = 0; k < sizeof(a); k++) {
            s = s * 6364136223846793005ULL + 1;
            a[k] = (char)('a' + (s >> 59));
        }
        a[n] = 0;
        memcpy(b, a, sizeof(a));
        memcpy(c, a, sizeof(a));
        size_t off = (size_t)(s >> 40) % 40, len = (size_t)(s >> 20) % 200;
        memmove(b + off, b + 3, len);
        f_memmove(c + off, c + 3, len);
        if (memcmp(b, c, sizeof(b))) {
            printf("FAIL memmove\n");
            failures++;
            break;
        }
        f_memmove(c + 3, c + off, len);
        memmove(b + 3, b + off, len);
        if (memcmp(b, c, sizeof(b)) || f_strlen(a) != strlen(a)) {
            printf("FAIL memmove/strlen\n");
            failures++;
            break;
        }
        f_memset(c + off, 'Q', len);
        memset(b + off, 'Q', len);
        f_memcpy(c + 1, a + 7, len);
        memcpy(b + 1, a + 7, len);
        if (memcmp(b, c, sizeof(b)) || (f_memcmp(a, b, n) > 0) != (memcmp(a, b, n) > 0)) {
            printf("FAIL memset/memcpy/memcmp\n");
            failures++;
            break;
        }
        b[299] = c[299] = 0;
        if ((f_strcmp(a, b) > 0) != (strcmp(a, b) > 0) || (f_strncmp(a, b, 5) == 0) != (strncmp(a, b, 5) == 0) ||
            f_strchr(a, 'c') != strchr(a, 'c') || f_strrchr(a, 'd') != strrchr(a, 'd') ||
            f_strstr(a, "ab") != strstr(a, "ab")) {
            printf("FAIL str* routines\n");
            failures++;
            break;
        }
        char d[16];
        if (f_strlcpy(d, a, sizeof(d)) != strlen(a) || strncmp(d, a, 15) || strlen(d) > 15) {
            printf("FAIL strlcpy\n");
            failures++;
            break;
        }
    }
    printf("test_fmt: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
