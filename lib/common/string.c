/* Fresh OS - string/memory routines shared by the kernel and libc. */
#include <stdint.h>
#include "fresh/str.h"

void *memcpy(void *dst, const void *src, size_t n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;
    if ((((uintptr_t)d | (uintptr_t)s) & 7) == 0) {
        while (n >= 8) {
            *(uint64_t *)d = *(const uint64_t *)s;
            d += 8;
            s += 8;
            n -= 8;
        }
    }
    while (n--)
        *d++ = *s++;
    return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;
    if (d == s || n == 0)
        return dst;
    if (d < s || d >= s + n)
        return memcpy(dst, src, n);
    d += n;
    s += n;
    if ((((uintptr_t)d | (uintptr_t)s) & 7) == 0) {
        while (n >= 8) {
            d -= 8;
            s -= 8;
            *(uint64_t *)d = *(const uint64_t *)s;
            n -= 8;
        }
    }
    while (n--)
        *--d = *--s;
    return dst;
}

void *memset(void *dst, int c, size_t n)
{
    unsigned char *d = dst;
    uint64_t v = (unsigned char)c;
    v |= v << 8;
    v |= v << 16;
    v |= v << 32;
    while (n && ((uintptr_t)d & 7)) {
        *d++ = (unsigned char)c;
        n--;
    }
    while (n >= 8) {
        *(uint64_t *)d = v;
        d += 8;
        n -= 8;
    }
    while (n--)
        *d++ = (unsigned char)c;
    return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *x = a, *y = b;
    for (size_t i = 0; i < n; i++)
        if (x[i] != y[i])
            return x[i] < y[i] ? -1 : 1;
    return 0;
}

size_t strlen(const char *s)
{
    size_t n = 0;
    while (s[n])
        n++;
    return n;
}

size_t strnlen(const char *s, size_t max)
{
    size_t n = 0;
    while (n < max && s[n])
        n++;
    return n;
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i] || !a[i])
            return (unsigned char)a[i] - (unsigned char)b[i];
    }
    return 0;
}

char *strcpy(char *dst, const char *src)
{
    char *d = dst;
    while ((*d++ = *src++))
        ;
    return dst;
}

char *strncpy(char *dst, const char *src, size_t n)
{
    size_t i = 0;
    for (; i < n && src[i]; i++)
        dst[i] = src[i];
    for (; i < n; i++)
        dst[i] = 0;
    return dst;
}

char *strcat(char *dst, const char *src)
{
    strcpy(dst + strlen(dst), src);
    return dst;
}

size_t strlcpy(char *dst, const char *src, size_t size)
{
    size_t len = strlen(src);
    if (size) {
        size_t n = len < size - 1 ? len : size - 1;
        memcpy(dst, src, n);
        dst[n] = 0;
    }
    return len;
}

size_t strlcat(char *dst, const char *src, size_t size)
{
    size_t dl = strnlen(dst, size);
    if (dl == size)
        return size + strlen(src);
    return dl + strlcpy(dst + dl, src, size - dl);
}

char *strchr(const char *s, int c)
{
    for (;; s++) {
        if (*s == (char)c)
            return (char *)s;
        if (!*s)
            return NULL;
    }
}

char *strrchr(const char *s, int c)
{
    const char *r = NULL;
    for (;; s++) {
        if (*s == (char)c)
            r = s;
        if (!*s)
            return (char *)r;
    }
}

char *strstr(const char *hay, const char *needle)
{
    size_t n = strlen(needle);
    if (!n)
        return (char *)hay;
    for (; *hay; hay++)
        if (*hay == *needle && !strncmp(hay, needle, n))
            return (char *)hay;
    return NULL;
}
