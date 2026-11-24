/* Fresh OS - printf-style formatting shared by the kernel and libc.
 * Supports: %d %i %u %x %X %o %p %s %c %% with flags '-', '0', ' ', '+',
 * width and precision (including '*'), and length modifiers hh h l ll z t j. */
#include <stdint.h>
#include "fresh/str.h"

struct out {
    char *buf;
    size_t size;
    size_t pos;
};

static void put(struct out *o, char c)
{
    if (o->pos + 1 < o->size)
        o->buf[o->pos] = c;
    o->pos++;
}

static void pad(struct out *o, char c, int n)
{
    while (n-- > 0)
        put(o, c);
}

static void emit_num(struct out *o, uint64_t v, int neg, unsigned base, int upper, int width, int prec, int left,
                     int zero, char sign_ch, int alt)
{
    char tmp[24];
    int n = 0;
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    if (v == 0 && prec != 0)
        tmp[n++] = '0';
    while (v) {
        tmp[n++] = digits[v % base];
        v /= base;
    }
    int zeros = prec > n ? prec - n : 0;
    char sign = neg ? '-' : sign_ch;
    int prefix = alt && base == 16 ? 2 : 0;
    int len = n + zeros + (sign ? 1 : 0) + prefix;
    if (!left && !(zero && prec < 0))
        pad(o, ' ', width - len);
    if (sign)
        put(o, sign);
    if (prefix) {
        put(o, '0');
        put(o, upper ? 'X' : 'x');
    }
    if (!left && zero && prec < 0)
        pad(o, '0', width - len);
    pad(o, '0', zeros);
    while (n)
        put(o, tmp[--n]);
    if (left)
        pad(o, ' ', width - len);
}

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
    struct out o = {buf, size, 0};
    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            put(&o, *fmt);
            continue;
        }
        fmt++;
        int left = 0, zero = 0, alt = 0;
        char sign_ch = 0;
        for (;; fmt++) {
            if (*fmt == '-')
                left = 1;
            else if (*fmt == '0')
                zero = 1;
            else if (*fmt == '+')
                sign_ch = '+';
            else if (*fmt == ' ') {
                if (!sign_ch)
                    sign_ch = ' ';
            } else if (*fmt == '#')
                alt = 1;
            else
                break;
        }
        int width = 0, prec = -1;
        if (*fmt == '*') {
            width = va_arg(ap, int);
            if (width < 0) {
                left = 1;
                width = -width;
            }
            fmt++;
        } else {
            while (*fmt >= '0' && *fmt <= '9')
                width = width * 10 + (*fmt++ - '0');
        }
        if (*fmt == '.') {
            fmt++;
            prec = 0;
            if (*fmt == '*') {
                prec = va_arg(ap, int);
                if (prec < 0)
                    prec = -1;
                fmt++;
            } else {
                while (*fmt >= '0' && *fmt <= '9')
                    prec = prec * 10 + (*fmt++ - '0');
            }
        }
        int lng = 0; /* 0 int, 1 long, 2 long long, -1 short, -2 char */
        for (;; fmt++) {
            if (*fmt == 'l')
                lng = lng >= 1 ? 2 : 1;
            else if (*fmt == 'h')
                lng = lng <= -1 ? -2 : -1;
            else if (*fmt == 'z' || *fmt == 't' || *fmt == 'j')
                lng = 2;
            else
                break;
        }
        char c = *fmt;
        if (!c)
            break;
        switch (c) {
        case 'd':
        case 'i': {
            int64_t v;
            if (lng >= 1)
                v = va_arg(ap, int64_t);
            else
                v = va_arg(ap, int);
            if (lng == -1)
                v = (short)v;
            else if (lng == -2)
                v = (signed char)v;
            uint64_t mag = v < 0 ? (uint64_t)0 - (uint64_t)v : (uint64_t)v;
            emit_num(&o, mag, v < 0, 10, 0, width, prec, left, zero, sign_ch, 0);
            break;
        }
        case 'u':
        case 'x':
        case 'X':
        case 'o': {
            uint64_t v;
            if (lng >= 1)
                v = va_arg(ap, uint64_t);
            else
                v = va_arg(ap, unsigned int);
            if (lng == -1)
                v = (unsigned short)v;
            else if (lng == -2)
                v = (unsigned char)v;
            unsigned base = c == 'u' ? 10 : c == 'o' ? 8 : 16;
            emit_num(&o, v, 0, base, c == 'X', width, prec, left, zero, 0, alt);
            break;
        }
        case 'p': {
            uint64_t v = (uint64_t)(uintptr_t)va_arg(ap, void *);
            emit_num(&o, v, 0, 16, 0, width, prec, left, zero, 0, 1);
            break;
        }
        case 'c': {
            char ch = (char)va_arg(ap, int);
            if (!left)
                pad(&o, ' ', width - 1);
            put(&o, ch);
            if (left)
                pad(&o, ' ', width - 1);
            break;
        }
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s)
                s = "(null)";
            int len = (int)(prec >= 0 ? strnlen(s, (size_t)prec) : strlen(s));
            if (!left)
                pad(&o, ' ', width - len);
            for (int i = 0; i < len; i++)
                put(&o, s[i]);
            if (left)
                pad(&o, ' ', width - len);
            break;
        }
        case '%':
            put(&o, '%');
            break;
        default:
            put(&o, '%');
            put(&o, c);
            break;
        }
    }
    if (size)
        o.buf[o.pos < size ? o.pos : size - 1] = 0;
    return (int)o.pos;
}

int snprintf(char *buf, size_t size, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return r;
}
