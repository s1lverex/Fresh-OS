/* Fresh OS libc - buffered stdio on file descriptors. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fresh.h>

static FILE f_in = {0, 0, 0, 0, 0, 0, {0}};
static FILE f_out = {1, 1, 0, 0, 0, 0, {0}};
static FILE f_err = {2, 1, 0, 0, 0, 0, {0}};
FILE *stdin = &f_in;
FILE *stdout = &f_out;
FILE *stderr = &f_err;
static int out_is_tty = -1;

int fflush(FILE *f)
{
    if (!f) {
        fflush(stdout);
        fflush(stderr);
        return 0;
    }
    if (f->mode != 1 || f->len == 0)
        return 0;
    size_t off = 0;
    while (off < f->len) {
        long w = write(f->fd, f->buf + off, f->len - off);
        if (w <= 0) {
            f->err = 1;
            f->len = 0;
            return EOF;
        }
        off += (size_t)w;
    }
    f->len = 0;
    return 0;
}

static int put_bytes(FILE *f, const char *s, size_t n)
{
    if (f->mode != 1)
        return EOF;
    if (f == stdout && out_is_tty < 0)
        out_is_tty = isatty(1);
    for (size_t i = 0; i < n; i++) {
        f->buf[f->len++] = (unsigned char)s[i];
        if (f->len == BUFSIZ && fflush(f))
            return EOF;
    }
    /* stderr is unbuffered, a terminal stdout is line buffered */
    if (f == stderr || (f == stdout && out_is_tty && memchr(s, '\n', n)))
        return fflush(f);
    return 0;
}

int vfprintf(FILE *f, const char *fmt, va_list ap)
{
    char small[512];
    va_list cp;
    va_copy(cp, ap);
    int n = vsnprintf(small, sizeof(small), fmt, ap);
    if (n < (int)sizeof(small)) {
        va_end(cp);
        return put_bytes(f, small, (size_t)n) ? -1 : n;
    }
    char *big = malloc((size_t)n + 1);
    if (!big) {
        va_end(cp);
        return -1;
    }
    vsnprintf(big, (size_t)n + 1, fmt, cp);
    va_end(cp);
    int r = put_bytes(f, big, (size_t)n) ? -1 : n;
    free(big);
    return r;
}

int printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = vfprintf(stdout, fmt, ap);
    va_end(ap);
    return r;
}

int fprintf(FILE *f, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = vfprintf(f, fmt, ap);
    va_end(ap);
    return r;
}

int dprintf(int fd, const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > (int)sizeof(buf) - 1)
        n = sizeof(buf) - 1;
    return (int)write(fd, buf, (size_t)n);
}

int fputs(const char *s, FILE *f) { return put_bytes(f, s, strlen(s)); }

int fputc(int c, FILE *f)
{
    char ch = (char)c;
    return put_bytes(f, &ch, 1) ? EOF : (unsigned char)c;
}

int putchar(int c) { return fputc(c, stdout); }

int puts(const char *s)
{
    if (fputs(s, stdout))
        return EOF;
    return fputc('\n', stdout) == EOF ? EOF : 0;
}

size_t fwrite(const void *p, size_t sz, size_t n, FILE *f)
{
    if (!sz || !n)
        return 0;
    return put_bytes(f, p, sz * n) ? 0 : n;
}

static int fill(FILE *f)
{
    if (f == stdin)
        fflush(stdout);
    long r = read(f->fd, f->buf, BUFSIZ);
    if (r <= 0) {
        if (r < 0)
            f->err = 1;
        else
            f->eof = 1;
        return EOF;
    }
    f->len = (size_t)r;
    f->pos = 0;
    return 0;
}

int fgetc(FILE *f)
{
    if (f->mode != 0)
        return EOF;
    if (f->pos >= f->len && fill(f))
        return EOF;
    return f->buf[f->pos++];
}

int getchar(void) { return fgetc(stdin); }

char *fgets(char *s, int n, FILE *f)
{
    if (n <= 0)
        return NULL;
    int i = 0;
    while (i < n - 1) {
        int c = fgetc(f);
        if (c == EOF)
            break;
        s[i++] = (char)c;
        if (c == '\n')
            break;
    }
    if (i == 0)
        return NULL;
    s[i] = 0;
    return s;
}

size_t fread(void *p, size_t sz, size_t n, FILE *f)
{
    size_t total = sz * n, got = 0;
    unsigned char *out = p;
    while (got < total) {
        if (f->pos >= f->len) {
            if (total - got >= BUFSIZ) { /* large read: bypass the buffer */
                long r = read(f->fd, out + got, total - got);
                if (r <= 0) {
                    if (r < 0)
                        f->err = 1;
                    else
                        f->eof = 1;
                    break;
                }
                got += (size_t)r;
                continue;
            }
            if (fill(f))
                break;
        }
        size_t k = f->len - f->pos;
        if (k > total - got)
            k = total - got;
        memcpy(out + got, f->buf + f->pos, k);
        f->pos += k;
        got += k;
    }
    return sz ? got / sz : 0;
}

FILE *fdopen(int fd, const char *mode)
{
    FILE *f = calloc(1, sizeof(FILE));
    if (!f)
        return NULL;
    f->fd = fd;
    f->mode = (mode[0] == 'w' || mode[0] == 'a') ? 1 : 0;
    return f;
}

FILE *fopen(const char *path, const char *mode)
{
    int flags;
    if (mode[0] == 'r')
        flags = O_RDONLY;
    else if (mode[0] == 'w')
        flags = O_WRONLY | O_CREAT | O_TRUNC;
    else if (mode[0] == 'a')
        flags = O_WRONLY | O_CREAT | O_APPEND;
    else
        return NULL;
    int fd = open(path, flags);
    if (fd < 0)
        return NULL;
    FILE *f = fdopen(fd, mode);
    if (!f)
        close(fd);
    return f;
}

int fclose(FILE *f)
{
    int r = fflush(f);
    close(f->fd);
    if (f != stdin && f != stdout && f != stderr)
        free(f);
    return r;
}

int feof(FILE *f) { return f->eof; }
int ferror(FILE *f) { return f->err; }

void perror(const char *msg)
{
    fprintf(stderr, "%s: error\n", msg);
}
