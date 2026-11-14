#include <string.h>

#include "fmt.h"

static void put(char *out, long size, long *o, char c)
{
    if (*o < size - 1) out[*o] = c;
    (*o)++;
}

long mill_format(char *out, long size, const char *fmt, const long *args)
{
    long o = 0;
    for (; *fmt; fmt++) {
        char digits[12], conv;
        const char *s;
        long n, width = 0, prec = -1, len, pad;
        int left = 0, zero = 0, neg = 0;
        if (*fmt != '%') { put(out, size, &o, *fmt); continue; }
        fmt++;
        for (; *fmt == '-' || *fmt == '0'; fmt++) { if (*fmt == '-') left = 1; else zero = 1; }
        while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');
        if (*fmt == '.') {
            prec = 0;
            fmt++;
            if (*fmt == '*') { prec = *args++; fmt++; if (prec < 0) prec = -1; }
            else while (*fmt >= '0' && *fmt <= '9') prec = prec * 10 + (*fmt++ - '0');
        }
        if (*fmt == 'l') fmt++;
        conv = *fmt;
        if (!conv) break;
        switch (conv) {
        case 's':
            s = (const char *)*args++;
            if (!s) s = "";
            len = strlen(s);
            if (prec >= 0 && len > prec) len = prec;
            zero = 0;
            break;
        case 'c':
            digits[0] = (char)*args++;
            s = digits;
            len = 1;
            zero = 0;
            break;
        case 'd': case 'u': case 'x': {
            unsigned long v;
            int base = conv == 'x' ? 16 : 10, i = sizeof digits;
            n = *args++;
            if (conv == 'd' && n < 0) { neg = 1; v = 0UL - (unsigned long)n; }
            else v = (unsigned long)n;
            do { digits[--i] = "0123456789abcdef"[v % base]; v /= base; } while (v);
            s = digits + i;
            len = sizeof digits - i;
            break;
        }
        default:
            put(out, size, &o, conv);
            continue;
        }
        pad = width - len - neg;
        if (neg && zero) put(out, size, &o, '-');
        if (!left) while (pad-- > 0) put(out, size, &o, zero ? '0' : ' ');
        if (neg && !zero) put(out, size, &o, '-');
        for (n = 0; n < len; n++) put(out, size, &o, s[n]);
        if (left) while (pad-- > 0) put(out, size, &o, ' ');
    }
    if (size > 0) out[o < size ? o : size - 1] = 0;
    return o;
}
