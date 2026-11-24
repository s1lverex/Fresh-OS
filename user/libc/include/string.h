#ifndef _STRING_H
#define _STRING_H
#include "fresh/str.h"

char *strdup(const char *s);
char *strndup(const char *s, size_t n);
char *strtok_r(char *s, const char *delim, char **save);
size_t strspn(const char *s, const char *accept);
size_t strcspn(const char *s, const char *reject);
int strcasecmp(const char *a, const char *b);
int strncasecmp(const char *a, const char *b, size_t n);
void *memchr(const void *s, int c, size_t n);
const char *strerror(int err);

#endif
