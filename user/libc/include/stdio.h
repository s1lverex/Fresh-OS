#ifndef _STDIO_H
#define _STDIO_H
#include <stddef.h>
#include <stdarg.h>
#include "fresh/str.h"

#define EOF (-1)
#define BUFSIZ 4096

typedef struct FILE {
    int fd;
    int mode; /* 0 read, 1 write */
    int err, eof;
    size_t len, pos;
    unsigned char buf[BUFSIZ];
} FILE;

extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;

int printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int fprintf(FILE *f, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
int vfprintf(FILE *f, const char *fmt, va_list ap);
int dprintf(int fd, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
int fputs(const char *s, FILE *f);
int fputc(int c, FILE *f);
int putchar(int c);
int puts(const char *s);
size_t fwrite(const void *p, size_t sz, size_t n, FILE *f);
size_t fread(void *p, size_t sz, size_t n, FILE *f);
int fflush(FILE *f);
int fgetc(FILE *f);
int getchar(void);
char *fgets(char *s, int n, FILE *f);
FILE *fopen(const char *path, const char *mode);
FILE *fdopen(int fd, const char *mode);
int fclose(FILE *f);
int feof(FILE *f);
int ferror(FILE *f);
void perror(const char *msg);

#endif
