#ifndef _STDLIB_H
#define _STDLIB_H
#include <stddef.h>

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1

void *malloc(size_t n);
void *calloc(size_t n, size_t sz);
void *realloc(void *p, size_t n);
void free(void *p);
__attribute__((noreturn)) void exit(int code);
__attribute__((noreturn)) void abort(void);
int atoi(const char *s);
long atol(const char *s);
long strtol(const char *s, char **end, int base);
unsigned long strtoul(const char *s, char **end, int base);
int abs(int x);
long labs(long x);
void qsort(void *base, size_t n, size_t sz, int (*cmp)(const void *, const void *));
int rand(void);
void srand(unsigned seed);
/* allocator statistics (bytes) for tests */
size_t malloc_in_use(void);

#endif
