/* Fresh OS libc - process start/exit, conversions, sorting, strings. */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <fresh.h>

int main(int argc, char **argv);

__attribute__((noreturn)) void __libc_start(int argc, char **argv)
{
    exit(main(argc, argv));
}

void exit(int code)
{
    fflush(stdout);
    fflush(stderr);
    sys_exit(code);
}

void abort(void)
{
    fflush(NULL);
    sys_exit(134);
}

long strtol(const char *s, char **end, int base)
{
    while (isspace((unsigned char)*s))
        s++;
    int neg = 0;
    if (*s == '-' || *s == '+')
        neg = *s++ == '-';
    unsigned long v = strtoul(s, end, base);
    return neg ? -(long)v : (long)v;
}

unsigned long strtoul(const char *s, char **end, int base)
{
    while (isspace((unsigned char)*s))
        s++;
    if ((base == 0 || base == 16) && s[0] == '0' && (s[1] == 'x' || s[1] == 'X') && isxdigit((unsigned char)s[2])) {
        s += 2;
        base = 16;
    } else if (base == 0) {
        base = s[0] == '0' && s[1] ? 8 : 10;
    }
    unsigned long v = 0;
    const char *start = s;
    for (;; s++) {
        int d;
        if (isdigit((unsigned char)*s))
            d = *s - '0';
        else if (isalpha((unsigned char)*s))
            d = tolower((unsigned char)*s) - 'a' + 10;
        else
            break;
        if (d >= base)
            break;
        v = v * (unsigned long)base + (unsigned long)d;
    }
    if (end)
        *end = (char *)(s == start ? start : s);
    return v;
}

int atoi(const char *s) { return (int)strtol(s, NULL, 10); }
long atol(const char *s) { return strtol(s, NULL, 10); }
int abs(int x) { return x < 0 ? -x : x; }
long labs(long x) { return x < 0 ? -x : x; }

static void swap_bytes(char *a, char *b, size_t n)
{
    while (n--) {
        char t = *a;
        *a++ = *b;
        *b++ = t;
    }
}

void qsort(void *base, size_t n, size_t sz, int (*cmp)(const void *, const void *))
{
    /* insertion sort for small inputs, otherwise heap sort (no recursion) */
    char *b = base;
    if (n < 2)
        return;
    if (n <= 16) {
        for (size_t i = 1; i < n; i++)
            for (size_t j = i; j > 0 && cmp(b + (j - 1) * sz, b + j * sz) > 0; j--)
                swap_bytes(b + (j - 1) * sz, b + j * sz, sz);
        return;
    }
    for (size_t start = n / 2; start-- > 0;) {
        size_t root = start;
        for (;;) {
            size_t child = 2 * root + 1;
            if (child >= n)
                break;
            if (child + 1 < n && cmp(b + child * sz, b + (child + 1) * sz) < 0)
                child++;
            if (cmp(b + root * sz, b + child * sz) >= 0)
                break;
            swap_bytes(b + root * sz, b + child * sz, sz);
            root = child;
        }
    }
    for (size_t end = n - 1; end > 0; end--) {
        swap_bytes(b, b + end * sz, sz);
        size_t root = 0;
        for (;;) {
            size_t child = 2 * root + 1;
            if (child >= end)
                break;
            if (child + 1 < end && cmp(b + child * sz, b + (child + 1) * sz) < 0)
                child++;
            if (cmp(b + root * sz, b + child * sz) >= 0)
                break;
            swap_bytes(b + root * sz, b + child * sz, sz);
            root = child;
        }
    }
}

static unsigned long rand_state = 1;
int rand(void)
{
    rand_state = rand_state * 6364136223846793005ULL + 1442695040888963407ULL;
    return (int)((rand_state >> 33) & 0x7FFFFFFF);
}
void srand(unsigned seed) { rand_state = seed; }

/* ---- string extras */
char *strdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *d = malloc(n);
    if (d)
        memcpy(d, s, n);
    return d;
}

char *strndup(const char *s, size_t n)
{
    size_t l = strnlen(s, n);
    char *d = malloc(l + 1);
    if (d) {
        memcpy(d, s, l);
        d[l] = 0;
    }
    return d;
}

size_t strspn(const char *s, const char *accept)
{
    size_t n = 0;
    while (s[n] && strchr(accept, s[n]))
        n++;
    return n;
}

size_t strcspn(const char *s, const char *reject)
{
    size_t n = 0;
    while (s[n] && !strchr(reject, s[n]))
        n++;
    return n;
}

char *strtok_r(char *s, const char *delim, char **save)
{
    if (!s)
        s = *save;
    s += strspn(s, delim);
    if (!*s) {
        *save = s;
        return NULL;
    }
    char *e = s + strcspn(s, delim);
    if (*e)
        *e++ = 0;
    *save = e;
    return s;
}

int strcasecmp(const char *a, const char *b)
{
    while (*a && tolower((unsigned char)*a) == tolower((unsigned char)*b)) {
        a++;
        b++;
    }
    return tolower((unsigned char)*a) - tolower((unsigned char)*b);
}

int strncasecmp(const char *a, const char *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        int x = tolower((unsigned char)a[i]), y = tolower((unsigned char)b[i]);
        if (x != y || !x)
            return x - y;
    }
    return 0;
}

void *memchr(const void *s, int c, size_t n)
{
    const unsigned char *p = s;
    for (size_t i = 0; i < n; i++)
        if (p[i] == (unsigned char)c)
            return (void *)(p + i);
    return NULL;
}
